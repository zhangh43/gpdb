/*-------------------------------------------------------------------------
 *
 * resgroup.c
 *	  GPDB resource group management code.
 *
 *
 * Copyright (c) 2006-2017, Greenplum inc.
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_resgroup.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "cdb/memquota.h"
#include "commands/resgroupcmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/pg_shmem.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/resgroup-ops.h"
#include "utils/resgroup.h"
#include "utils/resource_manager.h"
#include "utils/resowner.h"
#include "utils/session_state.h"
#include "utils/vmem_tracker.h"

#define InvalidSlotId	(-1)
#define RESGROUP_MAX_SLOTS	300

/*
 * Stock is a virtual unit used to manage the memory quota in resource groups.
 * The GPDB system is supposed to have RESGROUP_MAX_MEM_STOCKS stocks,
 * each resource group gets some stocks from the GPDB system according to
 * its memory_limit setting, each resource group slot gets some stocks
 * from the resource group according to the concurrency setting.
 *
 * We choose the name "stocks" instead of "shares" or "quota" because they
 * have other meanings in resource groups.
 */
#define RESGROUP_MAX_MEM_STOCKS		1000000LL

/*
 * GUC variables.
 */
char                		*gp_resgroup_memory_policy_str = NULL;
ResManagerMemoryPolicy     	gp_resgroup_memory_policy = RESMANAGER_MEMORY_POLICY_NONE;
bool						gp_log_resgroup_memory = false;
int							gp_resgroup_memory_policy_auto_fixed_mem;
bool						gp_resgroup_print_operator_memory_limits = false;

/*
 * Data structures
 */

typedef struct ResGroupHashEntry
{
	Oid		groupId;
	int		index;
} ResGroupHashEntry;

/*
 * Resource group config snapshot.
 *
 * All memory & cpu configs are in percentage.
 */
typedef struct ResGroupConfigSnapshot
{
	int		concurrency;
	int		memoryLimit;
	int		sharedQuota;
	int		spillRatio;
} ResGroupConfigSnapshot;

/*
 * Per proc resource group information.
 *
 * Config snapshot and runtime accounting information in current proc.
 */
typedef struct ResGroupProcData
{
	Oid		groupId;
	int		slotId;

	ResGroupConfigSnapshot config;

	uint32	memUsage;			/* memory usage of current proc */
	bool	doMemCheck;			/* whether to do memory limit check */
} ResGroupProcData;

/*
 * Per slot resource group information.
 *
 * Resource group have 'concurrency' number of slots.
 * Each transaction acquires a slot on master before running.
 * The information shared by QE processes on each segments are stored
 * in this structure.
 */
typedef struct ResGroupSlotData
{
	int				sessionId;

	ResGroupConfigSnapshot config;

	int32			segmentChunks;	/* total memory in chunks for segment */

	int32			memLimit;	/* memory limit of current resource group */
	int32			memSharedQuota;	/* shared memory quota of current resource group */

	int32			memStocks;	/* memory stocks of current slot */
	int32			memQuota;	/* memory quota of current slot */
	int32			memSpill;	/* memory spill of current slot */
	int32			memUsage;	/* total memory usage of procs belongs to this slot */
	int				nProcs;		/* number of procs in this slot */
	bool			inUse;
} ResGroupSlotData;

/*
 * Resource group information.
 */
typedef struct ResGroupData
{
	Oid			groupId;		/* Id for this group */
	int			nRunning;		/* number of running trans */
	PROC_QUEUE	waitProcs;
	int			totalExecuted;	/* total number of executed trans */
	int			totalQueued;	/* total number of queued trans	*/
	Interval	totalQueuedTime;/* total queue time */

	bool		lockedForDrop;  /* true if resource group is dropped but not committed yet */

	int32		memStocksGranted;	/* memory stocks granted to all the running slots */

	/*
	 * memory usage of this group, should always equal to the
	 * sum of session memory(session_state->sessionVmem) that
	 * belongs to this group
	 */
	int32		memUsage;
	int32		memSharedUsage;

	ResGroupSlotData slots[RESGROUP_MAX_SLOTS];
} ResGroupData;

typedef struct ResGroupControl
{
	HTAB			*htbl;
	int 			segmentsOnMaster;

	/*
	 * The hash table for resource groups in shared memory should only be populated
	 * once, so we add a flag here to implement this requirement.
	 */
	bool			loaded;

	int				nGroups;
	ResGroupData	groups[1];
} ResGroupControl;

/* GUC */
int		MaxResourceGroups;

/* static variables */

static ResGroupData	*MyResGroupSharedInfo = NULL;

static ResGroupControl *pResGroupControl = NULL;

static ResGroupProcData _MyResGroupProcInfo =
{
	InvalidOid, InvalidSlotId,
};
static ResGroupProcData *MyResGroupProcInfo = &_MyResGroupProcInfo;

static bool localResWaiting = false;

/* static functions */

static ResGroupData *ResGroupHashNew(Oid groupId);
static ResGroupData *ResGroupHashFind(Oid groupId);
static bool ResGroupHashRemove(Oid groupId);
static void ResGroupWait(ResGroupData *group, bool isLocked);
static bool ResGroupCreate(Oid groupId);
static void AtProcExit_ResGroup(int code, Datum arg);
static void ResGroupWaitCancel(void);
static int memSharedUsage2Percent(const ResGroupOpts *opts, int32 memSharedUsage);
static int memStocks2Percent(const ResGroupOpts *opts, int32 memStocks);
static bool validMemUsage(ResGroupData *group, const ResGroupOpts *opts);
static void attachToSlot(ResGroupData *group,
						 ResGroupSlotData *slot,
						 ResGroupProcData *self);
static void detachFromSlot(ResGroupData *group,
						   ResGroupSlotData *slot,
						   ResGroupProcData *self);
static int getFreeSlot(ResGroupData *group);
static int getSlot(ResGroupData *group);
static void putSlot(ResGroupData *group, int slotId);
static int ResGroupSlotAcquire(void);
static void addTotalQueueDuration(ResGroupData *group);
static void ResGroupSlotRelease(void);


/*
 * Estimate size the resource group structures will need in
 * shared memory.
 */
Size
ResGroupShmemSize(void)
{
	Size		size = 0;

	/* The hash of groups. */
	size = hash_estimate_size(MaxResourceGroups, sizeof(ResGroupHashEntry));

	/* The control structure. */
	size = add_size(size, sizeof(ResGroupControl) - sizeof(ResGroupData));

	/* The control structure. */
	size = add_size(size, mul_size(MaxResourceGroups, sizeof(ResGroupData)));

	/* Add a safety margin */
	size = add_size(size, size / 10);

	return size;
}

/*
 * Initialize the global ResGroupControl struct of resource groups.
 */
void
ResGroupControlInit(void)
{
	int			i;
    bool        found;
    HASHCTL     info;
    int         hash_flags;
	int			size;

	size = sizeof(*pResGroupControl) - sizeof(ResGroupData);
	size += mul_size(MaxResourceGroups, sizeof(ResGroupData));

    pResGroupControl = ShmemInitStruct("global resource group control",
                                       size, &found);
    if (found)
        return;
    if (pResGroupControl == NULL)
        goto error_out;

    /* Set key and entry sizes of hash table */
    MemSet(&info, 0, sizeof(info));
    info.keysize = sizeof(Oid);
    info.entrysize = sizeof(ResGroupHashEntry);
    info.hash = tag_hash;

    hash_flags = (HASH_ELEM | HASH_FUNCTION);

    LOG_RESGROUP_DEBUG(LOG, "Creating hash table for %d resource groups", MaxResourceGroups);

    pResGroupControl->htbl = ShmemInitHash("Resource Group Hash Table",
                                           MaxResourceGroups,
                                           MaxResourceGroups,
                                           &info, hash_flags);

    if (!pResGroupControl->htbl)
        goto error_out;

    /*
     * No need to acquire LWLock here, since this is expected to be called by
     * postmaster only
     */
    pResGroupControl->loaded = false;
    pResGroupControl->nGroups = MaxResourceGroups;

	for (i = 0; i < MaxResourceGroups; i++)
		pResGroupControl->groups[i].groupId = InvalidOid;

    return;

error_out:
	ereport(FATAL,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("not enough shared memory for resource group control")));
}

/*
 * Allocate a resource group entry from a hash table
 */
void
AllocResGroupEntry(Oid groupId)
{
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	bool groupOK = ResGroupCreate(groupId);
	if (!groupOK)
	{
		LWLockRelease(ResGroupLock);

		ereport(PANIC,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("not enough shared memory for resource groups")));
	}

	LWLockRelease(ResGroupLock);
}

/*
 * Remove a resource group entry from the hash table
 */
void
FreeResGroupEntry(Oid groupId)
{
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

#ifdef USE_ASSERT_CHECKING
	bool groupOK = 
#endif
		ResGroupHashRemove(groupId);
	Assert(groupOK);

	LWLockRelease(ResGroupLock);
}

/*
 * Load the resource groups in shared memory. Note this
 * can only be done after enough setup has been done. This uses
 * heap_open etc which in turn requires shared memory to be set up.
 */
void
InitResGroups(void)
{
	HeapTuple	tuple;
	SysScanDesc	sscan;
	int			numGroups;
	CdbComponentDatabases *cdbComponentDBs;
	CdbComponentDatabaseInfo *qdinfo;
	ResGroupCaps		caps;

	on_shmem_exit(AtProcExit_ResGroup, 0);
	if (pResGroupControl->loaded)
		return;
	/*
	 * Need a resource owner to keep the heapam code happy.
	 */
	Assert(CurrentResourceOwner == NULL);
	ResourceOwner owner = ResourceOwnerCreate(NULL, "InitResGroups");
	CurrentResourceOwner = owner;

	/*
	 * The resgroup shared mem initialization must be serialized. Only the first session
	 * should do the init.
	 * Serialization is done by LW_EXCLUSIVE ResGroupLock. However, we must obtain all DB
	 * locks before obtaining LWlock to prevent deadlock.
	 */
	Relation relResGroup = heap_open(ResGroupRelationId, AccessShareLock);
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	if (pResGroupControl->loaded)
		goto exit;

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		cdbComponentDBs = getCdbComponentDatabases();
		qdinfo = &cdbComponentDBs->entry_db_info[0];
		pResGroupControl->segmentsOnMaster = qdinfo->hostSegs;
		Assert(pResGroupControl->segmentsOnMaster > 0);
	}

	ResGroupOps_Init();

	numGroups = 0;
	sscan = systable_beginscan(relResGroup, InvalidOid, false, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		int cpuRateLimit;
		Oid groupId = HeapTupleGetOid(tuple);
		bool groupOK = ResGroupCreate(groupId);

		GetResGroupCapabilities(groupId, &caps);
		cpuRateLimit = caps.cpuRateLimit.value;

		if (!groupOK)
			ereport(PANIC,
					(errcode(ERRCODE_OUT_OF_MEMORY),
			 		errmsg("not enough shared memory for resource groups")));

		ResGroupOps_CreateGroup(groupId);
		ResGroupOps_SetCpuRateLimit(groupId, cpuRateLimit);

		numGroups++;
		Assert(numGroups <= MaxResourceGroups);
	}
	systable_endscan(sscan);

	pResGroupControl->loaded = true;
	LOG_RESGROUP_DEBUG(LOG, "initialized %d resource groups", numGroups);

exit:
	LWLockRelease(ResGroupLock);
	heap_close(relResGroup, AccessShareLock);
	CurrentResourceOwner = NULL;
	ResourceOwnerDelete(owner);
}

/*
 * Check resource group status when DROP RESOURCE GROUP
 *
 * Errors out if there're running transactions, otherwise lock the resource group.
 * New transactions will be queued if the resource group is locked.
 */
void
ResGroupCheckForDrop(Oid groupId, char *name)
{
	ResGroupData	*group;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory", groupId)));
	}

	if (group->nRunning > 0)
	{
		int nQuery = group->nRunning + group->waitProcs.size;
		LWLockRelease(ResGroupLock);

		Assert(name != NULL);
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("Cannot drop resource group \"%s\"", name),
				 errhint(" The resource group is currently managing %d query(ies) and cannot be dropped.\n"
						 "\tTerminate the queries first or try dropping the group later.\n"
						 "\tThe view pg_stat_activity tracks the queries managed by resource groups.", nQuery)));
	}
	group->lockedForDrop = true;

	LWLockRelease(ResGroupLock);
}

/*
 * Wake up the backends in the wait queue when DROP RESOURCE GROUP finishes.
 * Unlock the resource group if the transaction is abortted.
 * Remove the resource group entry in shared memory if the transaction is committed.
 *
 * This function is called in the callback function of DROP RESOURCE GROUP.
 */
void
ResGroupDropCheckForWakeup(Oid groupId, bool isCommit)
{
	int wakeNum;
	PROC_QUEUE	*waitQueue;
	ResGroupData	*group;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("Cannot find resource group %d in shared memory", groupId)));
	}

	Assert(group->lockedForDrop);

	waitQueue = &(group->waitProcs);
	wakeNum = waitQueue->size;

	while (wakeNum > 0)
	{
		PGPROC *waitProc;

		/* wake up one process in the wait queue */
		waitProc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
		SHMQueueDelete(&(waitProc->links));
		waitQueue->size--;

		waitProc->resWaiting = false;
		waitProc->resGranted = false;
		waitProc->resSlotId = InvalidSlotId;
		SetLatch(&waitProc->procLatch);
		wakeNum--;
	}

	if (isCommit)
	{
#ifdef USE_ASSERT_CHECKING
		bool groupOK = 
#endif
			ResGroupHashRemove(groupId);
		Assert(groupOK);
	}
	else
	{
		group->lockedForDrop = false;
	}

	LWLockRelease(ResGroupLock);
}

/*
 * Wake up the backends in the wait queue when 'concurrency' is increased.
 * This function is called in the callback function of ALTER RESOURCE GROUP.
 */
void
ResGroupAlterCheckForWakeup(Oid groupId)
{
	PROC_QUEUE	*waitQueue;
	ResGroupData	*group;

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("Cannot find resource group %d in shared memory", groupId)));
	}

	waitQueue = &group->waitProcs;

	while (waitQueue->size > 0)
	{
		PGPROC		*waitProc;
		int			slotId;

		slotId = getSlot(group);
		if (slotId == InvalidSlotId)
			break;

		/* wake up one process in the wait queue */
		waitProc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
		SHMQueueDelete(&(waitProc->links));
		waitQueue->size--;

		waitProc->resWaiting = false;
		waitProc->resGranted = true;
		waitProc->resSlotId = slotId;
		SetLatch(&waitProc->procLatch);
	}

	LWLockRelease(ResGroupLock);
}

/*
 *  Retrieve statistic information of type from resource group
 */
Datum
ResGroupGetStat(Oid groupId, ResGroupStatType type)
{
	ResGroupData	*group;
	Datum result;

	Assert(IsResGroupEnabled());

	LWLockAcquire(ResGroupLock, LW_SHARED);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory", groupId)));
	}

	switch (type)
	{
		case RES_GROUP_STAT_NRUNNING:
			result = Int32GetDatum(group->nRunning);
			break;
		case RES_GROUP_STAT_NQUEUEING:
			result = Int32GetDatum(group->waitProcs.size);
			break;
		case RES_GROUP_STAT_TOTAL_EXECUTED:
			result = Int32GetDatum(group->totalExecuted);
			break;
		case RES_GROUP_STAT_TOTAL_QUEUED:
			result = Int32GetDatum(group->totalQueued);
			break;
		case RES_GROUP_STAT_TOTAL_QUEUE_TIME:
			result = IntervalPGetDatum(&group->totalQueuedTime);
			break;
		case RES_GROUP_STAT_MEM_USAGE:
			result = Int32GetDatum(VmemTracker_ConvertVmemChunksToMB(group->memUsage));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Invalid stat type %d", type)));
	}

	LWLockRelease(ResGroupLock);

	return result;
}

/*
 * Dump memory information for current resource group.
 */
void
ResGroupDumpMemoryInfo(void)
{
	ResGroupSlotData	*slot;
	ResGroupProcData	*procInfo = MyResGroupProcInfo;
	ResGroupData		*sharedInfo = MyResGroupSharedInfo;

	if (sharedInfo)
	{
		Assert(procInfo->slotId != InvalidSlotId);

		slot = &sharedInfo->slots[procInfo->slotId];

		write_log("Resource group memory information: "
				  "group memory limit is %d MB, "
				  "shared quota in current slot is %d MB, "
				  "memory quota in current slot is %d MB, "
				  "memory usage in current resource group is %d MB, "
				  "memory usage in current slot is %d MB, "
				  "memory usage in current proc is %d MB",
				  VmemTracker_ConvertVmemChunksToMB(slot->memLimit),
				  VmemTracker_ConvertVmemChunksToMB(slot->memSharedQuota),
				  VmemTracker_ConvertVmemChunksToMB(slot->memQuota),
				  VmemTracker_ConvertVmemChunksToMB(sharedInfo->memUsage),
				  VmemTracker_ConvertVmemChunksToMB(slot->memUsage),
				  VmemTracker_ConvertVmemChunksToMB(procInfo->memUsage));
	}
	else
	{
		write_log("Resource group memory information: "
				  "memory usage in current proc is %d MB",
				  VmemTracker_ConvertVmemChunksToMB(procInfo->memUsage));
	}
}

/*
 * Reserve 'memoryChunks' number of chunks for current resource group.
 * It will first try to reserve memory from the resource group slot; if the slot
 * quota exceeded, it will reserve memory from the shared zone. It fails if the
 * shared quota is also exceeded, and no memory is reserved.
 *
 * 'overuseChunks' number of chunks can be overused for error handling,
 * in such a case waiverUsed is marked as true.
 */
bool
ResGroupReserveMemory(int32 memoryChunks, int32 overuseChunks, bool *waiverUsed)
{
	int32				slotMemUsage;
	int32				slotMemSharedNeeded;
	ResGroupSlotData	*slot;
	ResGroupProcData	*procInfo = MyResGroupProcInfo;
	ResGroupData		*sharedInfo = MyResGroupSharedInfo;

	if (!IsResGroupEnabled())
		return true;

	Assert(memoryChunks >= 0);

	/*
	 * Bypass the limit check when we are not in a valid resource group.
	 * But will update the memory usage of this proc, and it will be added up
	 * when this proc is assigned to a valid resource group.
	 */
	procInfo->memUsage += memoryChunks;
	if (!procInfo->doMemCheck)
		return true;

	if (sharedInfo->groupId != procInfo->groupId)
	{
		if (Debug_resource_group)
			write_log("Resource group is concurrently dropped while reserving memory: "
					  "dropped group=%d, my group=%d",
					  sharedInfo->groupId, procInfo->groupId);
		MyResGroupSharedInfo = NULL;
		procInfo->doMemCheck = false;
		return true;
	}

	Assert(sharedInfo != NULL);
	Assert(sharedInfo->groupId != InvalidOid);
	Assert(procInfo->slotId != InvalidSlotId);
	Assert(sharedInfo->memUsage >= 0);
	Assert(procInfo->memUsage >= 0);

	slot = &sharedInfo->slots[procInfo->slotId];

	/* reserve from slot memory */
	slotMemUsage = pg_atomic_add_fetch_u32((pg_atomic_uint32 *)&slot->memUsage, memoryChunks);
	slotMemSharedNeeded = slotMemUsage - slot->memQuota;

	if (slotMemSharedNeeded > 0)
	{
		int32 total;

		slotMemSharedNeeded = Min(slotMemSharedNeeded, memoryChunks);

		/* reserve from shared zone */
		total = pg_atomic_add_fetch_u32((pg_atomic_uint32 *)&sharedInfo->memSharedUsage,
										slotMemSharedNeeded);

		if (CritSectionCount == 0 &&
			total > slot->memSharedQuota + overuseChunks)
		{
			int32		oldUsage;

			oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&sharedInfo->memSharedUsage,
										slotMemSharedNeeded);
			Assert(oldUsage >= slotMemSharedNeeded);

			oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&slot->memUsage,
										memoryChunks);
			Assert(oldUsage >= memoryChunks);

			Assert(procInfo->memUsage >= memoryChunks);
			procInfo->memUsage -= memoryChunks;

			if (overuseChunks == 0)
				ResGroupDumpMemoryInfo();

			return false;
		}
		else if (CritSectionCount == 0 && total > slot->memSharedQuota)
		{
			*waiverUsed = true;
		}
	}

	/* update memory usage of current resource group */
	pg_atomic_add_fetch_u32((pg_atomic_uint32 *)&sharedInfo->memUsage,
							memoryChunks);

	return true;
}

/*
 * Release the memory of resource group
 */
void
ResGroupReleaseMemory(int32 memoryChunks)
{
	int32				sharedMemoryUsage;
	ResGroupSlotData	*slot;
	ResGroupProcData	*procInfo = MyResGroupProcInfo;
	ResGroupData		*sharedInfo = MyResGroupSharedInfo;
	int32				oldUsage;

	if (!IsResGroupEnabled())
		return;

	Assert(memoryChunks >= 0);
	Assert(memoryChunks <= procInfo->memUsage);

	procInfo->memUsage -= memoryChunks;
	if (!procInfo->doMemCheck)
		return;

	if (sharedInfo->groupId != procInfo->groupId)
	{
		if (Debug_resource_group)
			write_log("Resource group is concurrently dropped while releasing memory: "
					  "dropped group=%d, my group=%d",
					  sharedInfo->groupId, procInfo->groupId);
		MyResGroupSharedInfo = NULL;
		procInfo->doMemCheck = false;
		return;
	}

	Assert(sharedInfo != NULL);
	Assert(sharedInfo->groupId != InvalidOid);
	Assert(procInfo->slotId != InvalidSlotId);

	slot = &sharedInfo->slots[procInfo->slotId];
	sharedMemoryUsage = slot->memUsage - slot->memQuota;
	if (sharedMemoryUsage > 0)
	{
		int32 returnSize = Min(memoryChunks, sharedMemoryUsage);

		oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&sharedInfo->memSharedUsage,
									returnSize);
		Assert(oldUsage >= returnSize);
	}

	oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&slot->memUsage, memoryChunks);
	Assert(oldUsage >= memoryChunks);

	oldUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *)&sharedInfo->memUsage,
								memoryChunks);
	Assert(oldUsage >= memoryChunks);
}

/*
 * Decide the new resource group concurrency capabilities
 * of pg_resgroupcapability.
 *
 * The decision is based on current runtime information:
 * - 'proposed' will always be set to the latest setting;
 * - 'value' will be set to the most recent version of concurrency
 *   with which current nRunning doesn't exceed the limit;
 */
void
ResGroupDecideConcurrencyCaps(Oid groupId,
							  ResGroupCaps *caps,
							  const ResGroupOpts *opts)
{
	ResGroupData	*group;

	/* If resource group is not in use we can always pick the new settings. */
	if (!IsResGroupEnabled())
	{
		caps->concurrency.value = opts->concurrency;
		caps->concurrency.proposed = opts->concurrency;
		return;
	}

	LWLockAcquire(ResGroupLock, LW_SHARED);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory", groupId)));
	}

	/*
	 * If the runtime usage information doesn't exceed the new setting
	 * then we can pick this setting as the new 'value'.
	 */
	if (group->nRunning <= opts->concurrency)
		caps->concurrency.value = opts->concurrency;

	/* 'proposed' is always set with latest setting */
	caps->concurrency.proposed = opts->concurrency;

	LWLockRelease(ResGroupLock);
}

/*
 * Decide the new resource group memory capabilities
 * of pg_resgroupcapability.
 *
 * The decision is based on current runtime information:
 * - 'proposed' will always be set to the latest setting;
 * - 'value' will be set to the most recent version of memory settings
 *   with which current memory quota usage and memory shared usage
 *   doesn't exceed the limit;
 */
void
ResGroupDecideMemoryCaps(int groupId,
						 ResGroupCaps *caps,
						 const ResGroupOpts *opts)
{
	ResGroupData	*group;

	/* If resource group is not in use we can always pick the new settings. */
	if (!IsResGroupEnabled())
	{
		caps->memLimit.value = opts->memLimit;
		caps->memLimit.proposed = opts->memLimit;

		caps->memSharedQuota.value = opts->memSharedQuota;
		caps->memSharedQuota.proposed = opts->memSharedQuota;

		return;
	}

	LWLockAcquire(ResGroupLock, LW_SHARED);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Cannot find resource group with Oid %d in shared memory",
						groupId)));
	}

	/*
	 * If the runtime usage information doesn't exceed the new settings
	 * then we can pick these settings as the new 'value's.
	 */
	if (validMemUsage(group, opts))
	{
		caps->memLimit.value = opts->memLimit;
		caps->memSharedQuota.value = opts->memSharedQuota;
	}

	/* 'proposed' is always set with latest setting */
	caps->memSharedQuota.proposed = opts->memSharedQuota;
	caps->memLimit.proposed = opts->memLimit;

	LWLockRelease(ResGroupLock);
}

int
ResourceGroupGetQueryMemoryLimit(void)
{
	ResGroupSlotData	*slot;
	Assert(MyResGroupSharedInfo != NULL);
	Assert(MyResGroupProcInfo != NULL);
	Assert(MyResGroupProcInfo->slotId != InvalidSlotId);

	if (IsResManagerMemoryPolicyNone())
		return 0;

	slot = &MyResGroupSharedInfo->slots[MyResGroupProcInfo->slotId];
	return slot->memSpill << VmemTracker_GetChunkSizeInBits();
}

/*
 * ResGroupCreate -- initialize the elements for a resource group.
 *
 * Notes:
 *	It is expected that the appropriate lightweight lock is held before
 *	calling this - unless we are the startup process.
 */
static bool
ResGroupCreate(Oid groupId)
{
	ResGroupData	*group;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	Assert(OidIsValid(groupId));

	group = ResGroupHashNew(groupId);
	if (group == NULL)
		return false;

	group->groupId = groupId;
	group->nRunning = 0;
	ProcQueueInit(&group->waitProcs);
	group->totalExecuted = 0;
	group->totalQueued = 0;
	group->memUsage = 0;
	group->memSharedUsage = 0;
	group->memStocksGranted = 0;
	memset(&group->totalQueuedTime, 0, sizeof(group->totalQueuedTime));
	group->lockedForDrop = false;
	memset(group->slots, 0, sizeof(group->slots));

	return true;
}

/*
 * Convert memSharedUsage to percentage.
 *
 * The calculation is based on the passed opts.
 *
 * When a resgroup's shared memory is fully used the percentage is 100%.
 */
static int
memSharedUsage2Percent(const ResGroupOpts *opts, int32 memSharedUsage)
{
	int				nsegments;
	int32			segmentChunks;
	int32			memLimitInChunks;
	int32			memSharedQuotaInChunks;
	int				percent;

	/*
	 * When memSharedUsage == 0 then return 0;
	 * When memSharedUsage < 0 then return a negative percentage.
	 *
	 * There is no need to check memSharedQuota in such cases.
	 */
	if (memSharedUsage <= 0)
		return memSharedUsage;

	/* Return -1 if memSharedQuota is 0 and memSharedUsage > 0 */
	if (!opts->memSharedQuota)
		return -1;

	nsegments = Gp_role == GP_ROLE_DISPATCH
		? pResGroupControl->segmentsOnMaster : host_segments;
	Assert(nsegments > 0);

	segmentChunks = ResGroupOps_GetTotalMemory() *
		gp_resource_group_memory_limit / nsegments;
	memLimitInChunks = segmentChunks * opts->memLimit / 100;
	memSharedQuotaInChunks = memLimitInChunks * opts->memSharedQuota / 100;
	Assert(memSharedQuotaInChunks > 0);
	percent = memSharedUsage * 100 / memSharedQuotaInChunks;

	return percent;
}

/*
 * Convert memStocks to percentage.
 *
 * The calculation is based on the passed opts.
 *
 * When a resgroup's stocks is fully consumed the percentage is 100%.
 */
static int
memStocks2Percent(const ResGroupOpts *opts, int32 memStocks)
{
	int32			groupMemStocks;
	int				percent;

	/*
	 * When memStocks == 0 then return 0;
	 * When memStocks < 0 then return a negative percentage.
	 */
	if (memStocks <= 0)
		return memStocks;

	groupMemStocks = RESGROUP_MAX_MEM_STOCKS *
		opts->memLimit * (100 - opts->memSharedQuota) / 100 / 100;
	Assert(groupMemStocks > 0);
	percent = memStocks * 100 / groupMemStocks;

	return percent;
}

/*
 * Check whether memStocks and memSharedUsage both have valid percentages
 * with the passed (memLimit, memSharedQuota).
 */
static bool
validMemUsage(ResGroupData *group, const ResGroupOpts *opts)
{
	int32			memStocks;
	int32			memSharedUsage;
	int				percent;

	memStocks = group->memStocksGranted;
	memSharedUsage = group->memSharedUsage;

	percent = memSharedUsage2Percent(opts, memSharedUsage);
	if (percent < 0 || percent > 100)
		return false;

	percent = memStocks2Percent(opts, memStocks);
	if (percent < 0 || percent > 100)
		return false;

	return true;
}

/*
 * Attach current proc to a resource group & slot.
 *
 * Current proc's memory usage will be added to the group & slot.
 */
static void
attachToSlot(ResGroupData *group,
			 ResGroupSlotData *slot,
			 ResGroupProcData *self)
{
	int32			slotMemUsage;
	int32			sharedMemUsage;

	pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &slot->nProcs, 1);

	/* Add proc memory accounting info to memUsage in slot */
	slotMemUsage = pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &slot->memUsage,
										   self->memUsage);

	/* Add proc memory accounting info to memSharedUsage in group */
	sharedMemUsage = slotMemUsage - slot->memQuota;
	if (sharedMemUsage > 0)
	{
		/* Decide how many shared memory is in use by proc */
		sharedMemUsage = Min(sharedMemUsage, self->memUsage);
		pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &group->memSharedUsage,
								sharedMemUsage);
	}

	/* Add proc memory accounting info to memUsage in group */
	pg_atomic_add_fetch_u32((pg_atomic_uint32 *) &group->memUsage,
							self->memUsage);
}

/*
 * Detach current proc from a resource group & slot.
 *
 * Current proc's memory usage will be substracted from the group & slot.
 */
static void
detachFromSlot(ResGroupData *group,
			   ResGroupSlotData *slot,
			   ResGroupProcData *self)
{
	int32			value;
	int32			slotMemUsage;
	int32			sharedMemUsage;

	/* Sub proc memory accounting info from memUsage in group */
	value = pg_atomic_sub_fetch_u32((pg_atomic_uint32 *) &group->memUsage,
									self->memUsage);
	Assert(value >= 0);

	/* Sub proc memory accounting info from memUsage in slot */
	slotMemUsage = pg_atomic_fetch_sub_u32((pg_atomic_uint32 *) &slot->memUsage,
										   self->memUsage);
	Assert(slotMemUsage >= self->memUsage);

	/* Sub proc memory accounting info from memSharedUsage in group */
	sharedMemUsage = slotMemUsage - slot->memQuota;
	if (sharedMemUsage > 0)
	{
		/* Decide how many shared memory is in use by proc */
		int32 returnSize = Min(self->memUsage, sharedMemUsage);

		value = pg_atomic_sub_fetch_u32((pg_atomic_uint32 *) &group->memSharedUsage,
										returnSize);
		Assert(value >= 0);
	}

	value = pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&slot->nProcs, 1);
	Assert(value >= 0);
}

/*
 * Get a free resource group slot.
 *
 * A free resource group slot has inUse == false, no other information is checked.
 */
static int
getFreeSlot(ResGroupData *group)
{
	int i;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	for (i = 0; i < RESGROUP_MAX_SLOTS; i++)
	{
		if (group->slots[i].inUse)
			continue;

		group->slots[i].inUse = true;
		return i;
	}

	Assert(false && "No free slot available");
	return InvalidSlotId;
}

/*
 * Get a slot with memory quota granted.
 *
 * A slot can be got with this function if there is enough memory quota
 * available and the concurrency limit is not reached.
 *
 * On success the memory quota is marked as granted, nRunning is increased
 * and the slot's inUse flag is also set, the slot id is returned.
 *
 * On failure nothing is changed and InvalidSlotId is returned.
 */
static int
getSlot(ResGroupData *group)
{
	ResGroupConfigSnapshot	config;
	ResGroupSlotData	*slot;
	int32				groupMemStocks;
	int32				slotMemStocks;
	int32				memStocksGranted;
	int32				segmentChunks;
	int32				memLimit;
	int					slotId;
	ResGroupCaps		caps;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	Assert(Gp_role == GP_ROLE_DISPATCH);

	Assert(group != NULL);
	Assert(group->groupId != InvalidOid);

	/* Get resgroup config snapshot */
	GetResGroupCapabilities(group->groupId, &caps);
	config.concurrency = caps.concurrency.proposed;
	config.memoryLimit = caps.memLimit.value;
	config.sharedQuota = caps.memSharedQuota.value;
	config.spillRatio = caps.memSpillRatio.value;

	/* First check if the concurrency limit is reached */
	Assert(config.concurrency > 0);

	if (group->nRunning >= config.concurrency)
		return InvalidSlotId;

	/* Then check for memory stocks */
	Assert(pResGroupControl->segmentsOnMaster > 0);

	groupMemStocks = RESGROUP_MAX_MEM_STOCKS * config.memoryLimit *
		(100 - config.sharedQuota) / 100 / 100;
	slotMemStocks = groupMemStocks / config.concurrency;

	Assert(slotMemStocks > 0);
	Assert(group->memStocksGranted >= 0);
	Assert(group->memStocksGranted <= groupMemStocks);

	memStocksGranted = pg_atomic_add_fetch_u32((pg_atomic_uint32*) &group->memStocksGranted,
											   slotMemStocks);

	if (memStocksGranted > groupMemStocks)
	{
		/* No enough memory quota available, give up */
		memStocksGranted = pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&group->memStocksGranted,
												   slotMemStocks);
		Assert(memStocksGranted >= 0);
		return InvalidSlotId;
	}

	/* Now actually get a free slot */
	slotId = getFreeSlot(group);
	Assert(slotId != InvalidSlotId);

	slot = &group->slots[slotId];
	Assert(slot->inUse);

	/* Grant the memory stocks to it */
	Assert(slot->memStocks == 0);
	slot->memStocks = slotMemStocks;

	/* Grant the memory quota to it */
	segmentChunks = ResGroupOps_GetTotalMemory()
		* gp_resource_group_memory_limit
		/ pResGroupControl->segmentsOnMaster;
	memLimit = segmentChunks * config.memoryLimit / 100;
	slot->memQuota = memLimit * (100 - config.sharedQuota) / config.concurrency / 100;

	/* Store the config snapshot to it */
	slot->config = config;

	/* And finally increase nRunning */
	pg_atomic_add_fetch_u32((pg_atomic_uint32*)&group->nRunning, 1);

	return slotId;
}

/*
 * Put back a slot.
 *
 * This will release a slot, its memory quota will be freed and
 * nRunning will be decreased.
 */
static void
putSlot(ResGroupData *group, int slotId)
{
	ResGroupSlotData	*slot;
#ifdef USE_ASSERT_CHECKING
	int32				memStocksGranted;
#endif

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	Assert(Gp_role == GP_ROLE_DISPATCH);
	Assert(group != NULL);
	Assert(group->memStocksGranted >= 0);
	Assert(group->nRunning > 0);
	Assert(slotId != InvalidSlotId);

	slot = &group->slots[slotId];

	Assert(slot->inUse);
	Assert(slot->memQuota > 0);
	Assert(slot->memStocks > 0);

	/* Return the memory quota granted to this slot */
#ifdef USE_ASSERT_CHECKING
	memStocksGranted =
#endif
		pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&group->memStocksGranted,
								slot->memStocks);
	Assert(memStocksGranted >= 0);

	/* Mark the slot as free */
	slot->memStocks = 0;
	slot->inUse = false;

	/* And finally decrease nRunning */
	pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&group->nRunning, 1);
}

/*
 * Acquire a resource group slot
 *
 * Call this function at the start of the transaction.
 */
static int
ResGroupSlotAcquire(void)
{
	ResGroupData	*group;
	ResGroupCaps	caps;
	Oid			groupId;
	int			concurrencyProposed;
	bool		retried = false;

	Assert(MyResGroupProcInfo->groupId == InvalidOid);

	groupId = GetResGroupIdForRole(GetUserId());
	if (groupId == InvalidOid)
		groupId = superuser() ? ADMINRESGROUP_OID : DEFAULTRESGROUP_OID;

	GetResGroupCapabilities(groupId, &caps);
	concurrencyProposed = caps.concurrency.proposed;

retry:
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	group = ResGroupHashFind(groupId);
	if (group == NULL)
	{
		LWLockRelease(ResGroupLock);

		MyResGroupProcInfo->groupId = InvalidOid;
		MyResGroupSharedInfo = NULL;

		if (retried)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("Resource group %d was concurrently dropped", groupId)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Cannot find resource group %d in shared memory", groupId)));
	}

	MyResGroupSharedInfo = group;

	/* wait on the queue if the group is locked for drop */
	if (group->lockedForDrop)
	{
		Assert(group->nRunning == 0);
		ResGroupWait(group, true);

		/* retry if the drop resource group transaction is finished */
		retried = true;
		goto retry;
	}

	/* acquire a slot */
	if (group->nRunning < concurrencyProposed)
	{
		/* should not been granted a slot yet */
		Assert(MyProc->resSlotId == InvalidSlotId);

		/* so try to get one directly */
		MyProc->resSlotId = getSlot(group);

		/* if can't get one */
		if (MyProc->resSlotId == InvalidSlotId)
		{
			/* then wait one from some others */
			ResGroupWait(group, true);
			LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

			Assert(MyProc->resSlotId != InvalidSlotId);
		}

		group->totalExecuted++;
		LWLockRelease(ResGroupLock);
		pgstat_report_resgroup(0, group->groupId);
		Assert(MyProc->resSlotId != InvalidSlotId);
		return MyProc->resSlotId;
	}

	/* We have to wait for the slot */
	ResGroupWait(group, false);

	/*
	 * The waking process has granted us the slot.
	 * Update the statistic information of the resource group.
	 */
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);
	group->totalExecuted++;
	addTotalQueueDuration(group);
	LWLockRelease(ResGroupLock);
	Assert(MyProc->resSlotId != InvalidSlotId);
	return MyProc->resSlotId;
}

/* Update the total queued time of this group */
static void
addTotalQueueDuration(ResGroupData *group)
{
	Assert(LWLockHeldExclusiveByMe(ResGroupLock));
	if (group == NULL)
		return;

	TimestampTz start = pgstat_fetch_resgroup_queue_timestamp();
	TimestampTz now = GetCurrentTimestamp();
	Datum durationDatum = DirectFunctionCall2(timestamptz_age, TimestampTzGetDatum(now), TimestampTzGetDatum(start));
	Datum sumDatum = DirectFunctionCall2(interval_pl, IntervalPGetDatum(&group->totalQueuedTime), durationDatum);
	memcpy(&group->totalQueuedTime, DatumGetIntervalP(sumDatum), sizeof(Interval));
}

/*
 * Release the resource group slot
 *
 * Call this function at the end of the transaction.
 */
static void
ResGroupSlotRelease(void)
{
	ResGroupData	*group;
	PROC_QUEUE	*waitQueue;
	PGPROC		*waitProc;

	group = MyResGroupSharedInfo;
	Assert(group != NULL);

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	waitQueue = &group->waitProcs;

	Assert(MyProc->resSlotId != InvalidSlotId);
	putSlot(group, MyProc->resSlotId);
	MyProc->resSlotId = InvalidSlotId;

	/*
	 * My slot is put back, then how many queueing queries should I wake up?
	 * Maybe zero, maybe one, maybe more, depends on how the resgroup's
	 * configuration were changed during our execution.
	 */
	while (waitQueue->size > 0)
	{
		int			slotId;

		slotId = getSlot(group);
		if (slotId == InvalidSlotId)
			break;

		/* wake up one process in the wait queue */
		waitProc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
		SHMQueueDelete(&waitProc->links);
		waitQueue->size--;
		waitProc->resGranted = true;
		waitProc->resSlotId = slotId;	/* pass the slot to new query */
		LWLockRelease(ResGroupLock);

		waitProc->resWaiting = false;
		SetLatch(&waitProc->procLatch);

		LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);
	}

	AssertImply(waitQueue->size == 0,
				waitQueue->links.next == MAKE_OFFSET(&waitQueue->links) &&
				waitQueue->links.prev == MAKE_OFFSET(&waitQueue->links));

	LWLockRelease(ResGroupLock);
}

/*
 * Serialize the resource group information that need to dispatch to segment.
 */
void
SerializeResGroupInfo(StringInfo str)
{
	int tmp;
	ResGroupConfigSnapshot config;
	ResGroupProcData *procInfo = MyResGroupProcInfo;

	if (MyResGroupProcInfo->groupId != InvalidOid)
		config = MyResGroupProcInfo->config;
	else
		MemSet(&config, 0, sizeof(config));

	tmp = htonl(procInfo->groupId);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(procInfo->groupId));

	tmp = htonl(procInfo->slotId);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(procInfo->slotId));

	tmp = htonl(config.concurrency);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(config.concurrency));

	tmp = htonl(config.memoryLimit);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(config.memoryLimit));

	tmp = htonl(config.sharedQuota);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(config.sharedQuota));

	tmp = htonl(config.spillRatio);
	appendBinaryStringInfo(str, (char *) &tmp, sizeof(config.spillRatio));
}

/*
 * Deserialize the resource group information dispatched by QD.
 */
void
DeserializeResGroupInfo(struct ResGroupConfigSnapshot *config,
						const char *buf, int len)
{
	int			tmp;
	const char	*ptr = buf;
	ResGroupProcData	*procInfo = MyResGroupProcInfo;

	Assert(len > 0);

	memcpy(&tmp, ptr, sizeof(procInfo->groupId));
	procInfo->groupId = ntohl(tmp);
	ptr += sizeof(procInfo->groupId);

	memcpy(&tmp, ptr, sizeof(procInfo->slotId));
	procInfo->slotId = ntohl(tmp);
	ptr += sizeof(procInfo->slotId);

	memcpy(&tmp, ptr, sizeof(config->concurrency));
	config->concurrency = ntohl(tmp);
	ptr += sizeof(config->concurrency);

	memcpy(&tmp, ptr, sizeof(config->memoryLimit));
	config->memoryLimit = ntohl(tmp);
	ptr += sizeof(config->memoryLimit);

	memcpy(&tmp, ptr, sizeof(config->sharedQuota));
	config->sharedQuota = ntohl(tmp);
	ptr += sizeof(config->sharedQuota);

	memcpy(&tmp, ptr, sizeof(config->spillRatio));
	config->spillRatio = ntohl(tmp);
	ptr += sizeof(config->spillRatio);

	Assert(len == ptr - buf);
}

/*
 * Check whether should assign resource group on master.
 */
bool
ShouldAssignResGroupOnMaster(void)
{
	return IsResGroupEnabled() &&
		IsNormalProcessingMode() &&
		Gp_role == GP_ROLE_DISPATCH &&
		!AmIInSIGUSR1Handler();
}

/*
 * On master, QD is assigned to a resource group at the beginning of a transaction.
 * It will first acquire a slot from the resource group, and then, it will get the
 * current capability snapshot, update the memory usage information, and add to
 * the corresponding cgroup.
 */
void
AssignResGroupOnMaster(void)
{
	ResGroupData		*sharedInfo;
	ResGroupSlotData	*slot;
	ResGroupProcData	*procInfo;
	int		slotId;
	Oid		groupId;

	Assert(Gp_role == GP_ROLE_DISPATCH);

	/* Acquire slot */
	slotId = ResGroupSlotAcquire();
	Assert(slotId != InvalidSlotId);
	Assert(MyResGroupSharedInfo != NULL);
	sharedInfo = MyResGroupSharedInfo;
	groupId = sharedInfo->groupId;
	Assert(groupId != InvalidOid);
	Assert(!MyResGroupProcInfo->doMemCheck);

	/* Init slot */
	slot = &sharedInfo->slots[slotId];
	Assert(slot->memQuota > 0);
	slot->sessionId = gp_session_id;
	slot->segmentChunks = ResGroupOps_GetTotalMemory()
		* gp_resource_group_memory_limit / pResGroupControl->segmentsOnMaster;
	slot->memLimit = slot->segmentChunks * slot->config.memoryLimit / 100;
	slot->memSharedQuota = slot->memLimit * slot->config.sharedQuota / 100;
	slot->memSpill = slot->memLimit * slot->config.spillRatio
		/ slot->config.concurrency / 100;
	pg_atomic_add_fetch_u32((pg_atomic_uint32*)&slot->nProcs, 1);
	Assert(slot->memLimit > 0);
	Assert(slot->memQuota > 0);

	/* Init MyResGroupProcInfo */
	procInfo = MyResGroupProcInfo;
	procInfo->groupId = groupId;
	procInfo->slotId = slotId;
	procInfo->config = slot->config;
	Assert(pResGroupControl != NULL);
	Assert(pResGroupControl->segmentsOnMaster > 0);

	attachToSlot(sharedInfo, slot, procInfo);

	/* Start memory limit checking */
	Assert(procInfo->groupId != InvalidOid);
	Assert(procInfo->slotId != InvalidSlotId);
	procInfo->doMemCheck = true;

	/* Add into cgroup */
	ResGroupOps_AssignGroup(sharedInfo->groupId, MyProcPid);
}

/*
 * Detach from a resource group at the end of the transaction.
 */
void
UnassignResGroupOnMaster(void)
{
	ResGroupData		*sharedInfo;
	ResGroupSlotData	*slot;
	ResGroupProcData	*procInfo;

	if (MyResGroupSharedInfo == NULL)
	{
		Assert(MyResGroupProcInfo->doMemCheck == false);
		return;
	}

	procInfo = MyResGroupProcInfo;
	sharedInfo = MyResGroupSharedInfo;

	Assert(sharedInfo->groupId != InvalidOid);
	Assert(procInfo->groupId == sharedInfo->groupId);
	Assert(procInfo->slotId != InvalidSlotId);

	/* Stop memory limit checking */
	procInfo->doMemCheck = false;

	slot = &sharedInfo->slots[procInfo->slotId];

	detachFromSlot(sharedInfo, slot, procInfo);

	/* Cleanup procInfo */
	if (procInfo->memUsage > 10)
		LOG_RESGROUP_DEBUG(LOG, "Idle proc memory usage: %d", procInfo->memUsage);
	procInfo->groupId = InvalidOid;
	procInfo->slotId = InvalidSlotId;

	/* Cleanup slotInfo */
	pg_atomic_sub_fetch_u32((pg_atomic_uint32*)&slot->nProcs, 1);

	/* Relesase the slot */
	ResGroupSlotRelease();

	/* Cleanup sharedInfo */
	MyResGroupSharedInfo = NULL;
}

/*
 * QEs are not assigned/unassigned to a resource group on segments for each
 * transaction, instead, they switch resource group when a new resource group
 * id or slot id is dispatched.
 */
void
SwitchResGroupOnSegment(const char *buf, int len)
{
	Oid		prevGroupId;
	int		prevSlotId;
	ResGroupConfigSnapshot	config;
	ResGroupData		*sharedInfo;
	ResGroupSlotData	*slot;
	ResGroupProcData	*procInfo;
	ResGroupData		*prevSharedInfo = NULL;
	ResGroupSlotData	*prevSlot = NULL;

	procInfo = MyResGroupProcInfo;
	prevGroupId = procInfo->groupId;
	prevSlotId = procInfo->slotId;

	/* Stop memory limit checking */
	procInfo->doMemCheck = false;

	DeserializeResGroupInfo(&config, buf, len);

	AssertImply(procInfo->groupId != InvalidOid,
				procInfo->slotId != InvalidSlotId);
	AssertImply(prevGroupId != InvalidOid,
				prevSlotId != InvalidSlotId);

	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	if (procInfo->groupId == InvalidOid)
	{
		prevSharedInfo = MyResGroupSharedInfo;
		MyResGroupSharedInfo = NULL;

		if (prevSharedInfo)
		{
			Assert(prevSharedInfo->groupId == prevGroupId);
			prevSlot = &prevSharedInfo->slots[prevSlotId];
			detachFromSlot(prevSharedInfo, prevSlot, procInfo);
		}
		else
		{
			Assert(prevGroupId == InvalidOid);
			Assert(prevSlotId == InvalidSlotId);
			Assert(MyResGroupSharedInfo == NULL);
		}

		LWLockRelease(ResGroupLock);
		return;
	}

	if (MyResGroupSharedInfo != NULL && MyResGroupSharedInfo->groupId != InvalidOid)
	{
		prevSharedInfo = MyResGroupSharedInfo;
		if (prevSharedInfo->groupId == prevGroupId)
		{
			/* previous resource group is valid and not dropped yet */
			prevSlot = &prevSharedInfo->slots[prevSlotId];
		}
		else
		{
			/* previous resource group is already dropped */
			prevSharedInfo = NULL;
			prevSlot = NULL;
			prevGroupId = InvalidOid;
			prevSlotId = InvalidSlotId;
		}
	}

	sharedInfo = ResGroupHashFind(procInfo->groupId);
	Assert(sharedInfo != NULL);

	LWLockRelease(ResGroupLock);

	/* Init MyResGroupProcInfo */
	Assert(host_segments > 0);
	Assert(config.concurrency > 0);
	Assert(procInfo->slotId != InvalidSlotId);
	procInfo->config = config;

	/* Init slot */
	slot = &sharedInfo->slots[procInfo->slotId];
	slot->sessionId = gp_session_id;
	slot->config = config;
	slot->segmentChunks = ResGroupOps_GetTotalMemory()
		* gp_resource_group_memory_limit / host_segments;
	slot->memLimit = slot->segmentChunks * slot->config.memoryLimit / 100;
	slot->memSharedQuota = slot->memLimit * slot->config.sharedQuota / 100;
	slot->memQuota = slot->memLimit
		* (100 - slot->config.sharedQuota)
		/ slot->config.concurrency
		/ 100;
	slot->memSpill = slot->memLimit
		* procInfo->config.spillRatio
		/ procInfo->config.concurrency
		/ 100;
	Assert(slot->memLimit > 0);
	Assert(slot->memQuota > 0);

	if (prevSharedInfo != sharedInfo || prevSlot != slot)
	{
		if (prevSharedInfo)
			detachFromSlot(prevSharedInfo, prevSlot, procInfo);

		attachToSlot(sharedInfo, slot, procInfo);
	}

	MyResGroupSharedInfo = sharedInfo;

	/* Start memory limit checking */
	Assert(procInfo->groupId != InvalidOid);
	Assert(procInfo->slotId != InvalidSlotId);
	procInfo->doMemCheck = true;

	/* Add into cgroup */
	ResGroupOps_AssignGroup(procInfo->groupId, MyProcPid);
}

/*
 * Wait on the queue of resource group
 */
static void
ResGroupWait(ResGroupData *group, bool isLocked)
{
	PGPROC *proc = MyProc, *headProc;
	PROC_QUEUE *waitQueue;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	proc->resWaiting = true;

	waitQueue = &(group->waitProcs);

	headProc = (PGPROC *) &(waitQueue->links);
	SHMQueueInsertBefore(&(headProc->links), &(proc->links));
	waitQueue->size++;

	if (!isLocked)
		group->totalQueued++;

	LWLockRelease(ResGroupLock);
	pgstat_report_resgroup(GetCurrentTimestamp(), group->groupId);

	/* similar to lockAwaited in ProcSleep for interrupt cleanup */
	localResWaiting = true;

	/*
	 * Make sure we have released all locks before going to sleep, to eliminate
	 * deadlock situations
	 */
	PG_TRY();
	{
		for (;;)
		{
			ResetLatch(&proc->procLatch);

			CHECK_FOR_INTERRUPTS();

			if (!proc->resWaiting)
				break;
			WaitLatch(&proc->procLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, -1);
		}
	}
	PG_CATCH();
	{
		ResGroupWaitCancel();
		PG_RE_THROW();
	}
	PG_END_TRY();

	localResWaiting = false;

	pgstat_report_waiting(PGBE_WAITING_NONE);
}

/*
 * ResGroupHashNew -- return a new (empty) group object to initialize.
 *
 * Notes
 *	The resource group lightweight lock (ResGroupLock) *must* be held for
 *	this operation.
 */
static ResGroupData *
ResGroupHashNew(Oid groupId)
{
	int			i;
	bool		found;
	ResGroupHashEntry *entry;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	if (groupId == InvalidOid)
		return NULL;

	for (i = 0; i < pResGroupControl->nGroups; i++)
	{
		if (pResGroupControl->groups[i].groupId == InvalidOid)
			break;
	}
	Assert(i < pResGroupControl->nGroups);

	entry = (ResGroupHashEntry *)
		hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_ENTER_NULL, &found);
	/* caller should test that the group does not exist already */
	Assert(!found);
	entry->index = i;

	return &pResGroupControl->groups[i];
}

/*
 * ResGroupHashFind -- return the group for a given oid.
 *
 * Notes
 *	The resource group lightweight lock (ResGroupLock) *must* be held for
 *	this operation.
 */
static ResGroupData *
ResGroupHashFind(Oid groupId)
{
	bool				found;
	ResGroupHashEntry	*entry;

	Assert(LWLockHeldByMe(ResGroupLock));

	entry = (ResGroupHashEntry *)
		hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_FIND, &found);
	if (!found)
		return NULL;

	Assert(entry->index < pResGroupControl->nGroups);
	return &pResGroupControl->groups[entry->index];
}


/*
 * ResGroupHashRemove -- remove the group for a given oid.
 *
 * Notes
 *	The resource group lightweight lock (ResGroupLock) *must* be held for
 *	this operation.
 */
static bool
ResGroupHashRemove(Oid groupId)
{
	bool		found;
	ResGroupHashEntry	*entry;
	ResGroupData		*group;

	Assert(LWLockHeldExclusiveByMe(ResGroupLock));

	entry = (ResGroupHashEntry*)hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_FIND, &found);
	if (!found)
		return false;

	group = &pResGroupControl->groups[entry->index];
	group->groupId = InvalidOid;

	hash_search(pResGroupControl->htbl, (void *) &groupId, HASH_REMOVE, &found);

	return true;
}

/* Process exit without waiting for slot or received SIGTERM */
static void
AtProcExit_ResGroup(int code, Datum arg)
{
	ResGroupWaitCancel();
}

/*
 * Handle the interrupt cases when waiting on the queue
 *
 * The proc may wait on the queue for a slot, or wait for the
 * DROP transaction to finish. In the first case, at the same time
 * we get interrupted (SIGINT or SIGTERM), we could have been
 * grantted a slot or not. In the second case, there's no running
 * transaction in the group. If the DROP transaction is finished
 * (commit or abort) at the same time as we get interrupted,
 * MyProc should have been removed from the wait queue, and the
 * ResGroupData entry may have been removed if the DROP is committed.
 */
static void
ResGroupWaitCancel(void)
{
	ResGroupData	*group;
	PROC_QUEUE	*waitQueue;
	PGPROC		*waitProc;

	/* Process exit without waiting for slot */
	group = MyResGroupSharedInfo;
	if (group == NULL || !localResWaiting)
		return;

	/* We are sure to be interrupted in the for loop of ResGroupWait now */
	LWLockAcquire(ResGroupLock, LW_EXCLUSIVE);

	waitQueue = &group->waitProcs;

	if (MyProc->links.next != INVALID_OFFSET)
	{
		/* Still waiting on the queue when get interrupted, remove myself from the queue */

		Assert(waitQueue->size > 0);
		Assert(MyProc->resWaiting);
		Assert(MyProc->resSlotId == InvalidSlotId);

		addTotalQueueDuration(group);

		SHMQueueDelete(&(MyProc->links));
		waitQueue->size--;
	}
	else if (MyProc->links.next == INVALID_OFFSET && MyProc->resGranted)
	{
		/* Woken up by a slot holder */

		Assert(MyProc->resSlotId != InvalidSlotId);
		putSlot(group, MyProc->resSlotId);
		MyProc->resSlotId = InvalidSlotId;

		group->totalExecuted++;
		addTotalQueueDuration(group);

		/*
		 * Similar as ResGroupSlotRelease(), how many pending queries to
		 * wake up depends on how many slots we can get.
		 */
		while (waitQueue->size > 0)
		{
			int			slotId;

			slotId = getSlot(group);
			if (slotId == InvalidSlotId)
				break;

			/* wake up one process on the wait queue */
			waitProc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
			SHMQueueDelete(&(waitProc->links));
			waitQueue->size--;
			waitProc->resGranted = true;
			waitProc->resWaiting = false;
			waitProc->resSlotId = slotId;	/* pass the slot to new query */
			SetLatch(&waitProc->procLatch);
		}

		AssertImply(waitQueue->size == 0,
					waitQueue->links.next == MAKE_OFFSET(&waitQueue->links) &&
					waitQueue->links.prev == MAKE_OFFSET(&waitQueue->links));
	}
	else
	{
		/*
		 * The transaction of DROP RESOURCE GROUP is finished,
		 * ResGroupSlotAcquire will do the retry.
		 */
	}

	LWLockRelease(ResGroupLock);
	localResWaiting = false;
	pgstat_report_waiting(PGBE_WAITING_NONE);
	MyResGroupSharedInfo = NULL;
}

void
ResGroupGetMemInfo(int *memLimit, int *slotQuota, int *sharedQuota)
{
	ResGroupProcData *procInfo = MyResGroupProcInfo;
	int totalMem = ResGroupOps_GetTotalMemory();
	int hostSegment = Gp_role == GP_ROLE_EXECUTE ? host_segments : pResGroupControl->segmentsOnMaster;
	int segmentMem = totalMem * gp_resource_group_memory_limit / hostSegment;

	*memLimit = segmentMem * procInfo->config.memoryLimit / 100;
	*slotQuota = *memLimit
		* (100 - procInfo->config.sharedQuota)
		/ procInfo->config.concurrency
		/ 100;
	*sharedQuota = *memLimit * procInfo->config.sharedQuota / 100;
}
