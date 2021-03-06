/*-------------------------------------------------------------------------
 *
 * gp_resgroup_helper.c
 *	  Helper functions for resource group.
 *
 * Copyright (c) 2017-Present Pivotal Software, Inc.
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "gp-libpq-fe.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "catalog/pg_resgroup.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbvars.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/resgroup-ops.h"
#include "utils/resgroup.h"
#include "utils/resource_manager.h"

typedef struct ResGroupStat
{
	Datum groupId;

	StringInfo cpuUsage;
	StringInfo memUsage;
} ResGroupStat;

typedef struct ResGroupStatCtx
{
	int nGroups;
	ResGroupStat groups[1];
} ResGroupStatCtx;

static void calcCpuUsage(StringInfoData *str, int ncores,
						 int64 usageBegin, TimestampTz timestampBegin,
						 int64 usageEnd, TimestampTz timestampEnd);
static void getResUsage(ResGroupStatCtx *ctx, Oid inGroupId);

static void
calcCpuUsage(StringInfoData *str, int ncores,
			 int64 usageBegin, TimestampTz timestampBegin,
			 int64 usageEnd, TimestampTz timestampEnd)
{
	int64 duration;
	long secs;
	int usecs;
	int64 usage;

	usage = usageEnd - usageBegin;

	TimestampDifference(timestampBegin, timestampEnd, &secs, &usecs);

	duration = secs * 1000000 + usecs;

	/*
	 * usage is the cpu time (nano seconds) obtained by this group
	 * in the time duration (micro seconds), so cpu time on one core
	 * can be calculated as:
	 *
	 *     usage / 1000 / duration / ncores
	 *
	 * To convert it to percentange we should multiple 100%.
	 */
	appendStringInfo(str, "\"%d\":%.2f",
					 GpIdentity.segindex,
					 (usage / 10.0 / duration / ncores));
}

/*
 * Get resource usage.
 *
 * On QD this function dispatch the request to all QEs, collecting both
 * QEs' and QD's resource usage.
 *
 * On QE this function only collect the resource usage on itself.
 *
 * Memory & cpu usage are returned in JSON format.
 */
static void
getResUsage(ResGroupStatCtx *ctx, Oid inGroupId)
{
	int64 *usages;
	TimestampTz *timestamps;
	int ncores;
	int i, j;

	usages = palloc(sizeof(*usages) * ctx->nGroups);
	timestamps = palloc(sizeof(*timestamps) * ctx->nGroups);

	ncores = ResGroupOps_GetCpuCores();

	for (j = 0; j < ctx->nGroups; j++)
	{
		ResGroupStat *row = &ctx->groups[j];
		Oid groupId = DatumGetObjectId(row->groupId);

		usages[j] = ResGroupOps_GetCpuUsage(groupId);
		timestamps[j] = GetCurrentTimestamp();
	}

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		CdbPgResults cdb_pgresults = {NULL, 0};
		StringInfoData buffer;

		initStringInfo(&buffer);
		appendStringInfo(&buffer,
						 "SELECT groupid, cpu_usage, memory_usage "
						 "FROM pg_resgroup_get_status(%d)",
						 inGroupId);

		CdbDispatchCommand(buffer.data, DF_WITH_SNAPSHOT, &cdb_pgresults);

		if (cdb_pgresults.numResults == 0)
			elog(ERROR, "pg_resgroup_get_status() didn't get back any resource statistic from the segDBs");

		for (i = 0; i < cdb_pgresults.numResults; i++)
		{
			struct pg_result *pg_result = cdb_pgresults.pg_results[i];

			/*
			 * Any error here should have propagated into errbuf, so we shouldn't
			 * ever see anything other that tuples_ok here.  But, check to be
			 * sure.
			 */
			if (PQresultStatus(pg_result) != PGRES_TUPLES_OK)
			{
				cdbdisp_clearCdbPgResults(&cdb_pgresults);
				elog(ERROR, "pg_resgroup_get_status(): resultStatus not tuples_Ok");
			}

			Assert(PQntuples(pg_result) == ctx->nGroups);
			for (j = 0; j < ctx->nGroups; j++)
			{
				const char *result;
				ResGroupStat *row = &ctx->groups[j];
				Oid groupId = pg_atoi(PQgetvalue(pg_result, j, 0),
									  sizeof(Oid), 0);

				Assert(groupId == row->groupId);

				if (row->memUsage->len == 0)
				{
					Datum d = ResGroupGetStat(groupId, RES_GROUP_STAT_MEM_USAGE);

					row->groupId = groupId;
					appendStringInfo(row->memUsage, "{\"%d\":%d",
									 GpIdentity.segindex, DatumGetInt32(d));

					appendStringInfo(row->cpuUsage, "{");
					calcCpuUsage(row->cpuUsage, ncores, usages[j], timestamps[j],
								 ResGroupOps_GetCpuUsage(groupId),
								 GetCurrentTimestamp());
				}

				result = PQgetvalue(pg_result, j, 1);
				appendStringInfo(row->cpuUsage, ", %s", result);

				result = PQgetvalue(pg_result, j, 2);
				appendStringInfo(row->memUsage, ", %s", result);

				if (i == cdb_pgresults.numResults - 1)
				{
					appendStringInfoChar(row->cpuUsage, '}');
					appendStringInfoChar(row->memUsage, '}');
				}
			}
		}

		cdbdisp_clearCdbPgResults(&cdb_pgresults);
	}
	else
	{
		pg_usleep(300000);

		for (j = 0; j < ctx->nGroups; j++)
		{
			ResGroupStat *row = &ctx->groups[j];
			Oid groupId = DatumGetObjectId(row->groupId);
			Datum d = ResGroupGetStat(groupId, RES_GROUP_STAT_MEM_USAGE);

			appendStringInfo(row->memUsage, "\"%d\":%d",
							 GpIdentity.segindex, DatumGetInt32(d));

			calcCpuUsage(row->cpuUsage, ncores, usages[j], timestamps[j],
						 ResGroupOps_GetCpuUsage(groupId),
						 GetCurrentTimestamp());
		}
	}
}

static int
compareRow(const void *ptr1, const void *ptr2)
{
	const ResGroupStat *row1 = (const ResGroupStat *) ptr1;
	const ResGroupStat *row2 = (const ResGroupStat *) ptr2;

	return row1->groupId - row2->groupId;
}

/*
 * Get status of resource groups
 */
Datum
pg_resgroup_get_status(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ResGroupStatCtx *ctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		int			nattr = 8;

		funcctx = SRF_FIRSTCALL_INIT();

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(nattr, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "groupid", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "num_running", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "num_queueing", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "num_queued", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "num_executed", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "total_queue_duration", INTERVALOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "cpu_usage", JSONOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "memory_usage", JSONOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		if (IsResGroupEnabled())
		{
			Relation	pg_resgroup_rel;
			SysScanDesc	sscan;
			HeapTuple	tuple;
			Oid			inGroupId = PG_GETARG_OID(0);

			int ctxsize = sizeof(ResGroupStatCtx) +
				sizeof(ResGroupStat) * (MaxResourceGroups - 1);

			(void) inGroupId;

			funcctx->user_fctx = palloc(ctxsize);
			ctx = (ResGroupStatCtx *) funcctx->user_fctx;

			pg_resgroup_rel = heap_open(ResGroupRelationId, AccessShareLock);

			sscan = systable_beginscan(pg_resgroup_rel, InvalidOid, false,
									   SnapshotNow, 0, NULL);
			while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
			{
				Oid oid = ObjectIdGetDatum(HeapTupleGetOid(tuple));

				if (inGroupId == InvalidOid || inGroupId == oid)
				{
					Assert(funcctx->max_calls < MaxResourceGroups);
					ctx->groups[funcctx->max_calls].cpuUsage = makeStringInfo();
					ctx->groups[funcctx->max_calls].memUsage = makeStringInfo();
					ctx->groups[funcctx->max_calls++].groupId = oid;

					if (inGroupId != InvalidOid)
						break;
				}
			}
			systable_endscan(sscan);
			heap_close(pg_resgroup_rel, AccessShareLock);

			ctx->nGroups = funcctx->max_calls;
			qsort(ctx->groups, ctx->nGroups, sizeof(ctx->groups[0]), compareRow);

			if (ctx->nGroups > 0)
				getResUsage(ctx, inGroupId);
		}

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	ctx = (ResGroupStatCtx *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		/* for each row */
		Datum		values[8];
		bool		nulls[8];
		HeapTuple	tuple;
		Oid			groupId;
		char		statVal[MAXDATELEN + 1];
		ResGroupStat *row = &ctx->groups[funcctx->call_cntr];

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));
		MemSet(statVal, 0, sizeof(statVal));

		values[0] = row->groupId;
		groupId = DatumGetObjectId(values[0]);

		if (Gp_role == GP_ROLE_DISPATCH)
		{
			values[1] = ResGroupGetStat(groupId, RES_GROUP_STAT_NRUNNING);
			values[2] = ResGroupGetStat(groupId, RES_GROUP_STAT_NQUEUEING);
			values[3] = ResGroupGetStat(groupId, RES_GROUP_STAT_TOTAL_QUEUED);
			values[4] = ResGroupGetStat(groupId, RES_GROUP_STAT_TOTAL_EXECUTED);
			values[5] = ResGroupGetStat(groupId, RES_GROUP_STAT_TOTAL_QUEUE_TIME);
		}
		else
		{
			nulls[1] = true;
			nulls[2] = true;
			nulls[3] = true;
			nulls[4] = true;
			nulls[5] = true;
		}

		values[6] = CStringGetTextDatum(row->cpuUsage->data);
		values[7] = CStringGetTextDatum(row->memUsage->data);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		/* nothing left */
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Get status of resource groups in key-value style
 */
Datum
pg_resgroup_get_status_kv(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		int			nattr = 3;

		funcctx = SRF_FIRSTCALL_INIT();

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(nattr, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "groupid", OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "prop", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "value", TEXTOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->max_calls = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		SRF_RETURN_DONE(funcctx);
	}
	else
	{
		/* nothing left */
		SRF_RETURN_DONE(funcctx);
	}
}

