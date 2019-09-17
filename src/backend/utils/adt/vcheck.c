#include "postgres.h"
#include "access/htup.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "cdb/cdbappendonlyam.h"
#include "cdb/cdbllize.h"
#include "parser/parse_oper.h"
#include "nodes/makefuncs.h"
#include "nodes/primnodes.h"
#include "nodes/plannodes.h"
#include "nodes/relation.h"
#include "optimizer/walkers.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/vcheck.h"


typedef struct VecTypeHashEntry
{
	Oid src;
	Oid dest;
}VecTypeHashEntry;

/* Map between the vectorized types and non-vectorized types */
static HTAB *hashMapN2V = NULL;


/*
 * We check the expressions tree recursively becuase the args can be a sub expression,
 * we must check the return type of sub expression to fit the parent expressions.
 * so the retType in Vectorized is a temporary values, after we check on expression,
 * we set the retType of this expression, and transfer this value to his parent.
 */
typedef struct VectorizedContext
{
	plan_tree_base_prefix base; /* Required prefix for plan_tree_walker/mutator */
	Oid retType;
	bool	 replace;
}VectorizedContext;

/*
 * Check all the expressions if they can be vectorized
 * NOTE: if an expressions is vectorized, we return false...,because we should check
 * all the expressions in the Plan node, if we return true, then the walker will be
 * over...
 */
static bool
CheckVectorizedExpression(Node *node, VectorizedContext *ctx)
{
	if(NULL == node)
		return false;

	if(is_plan_node(node))
		return false;

	//check the type of Var if it can be vectorized
	if(IsA(node, Var))
	{
		Var *var = (Var*)node;
		Oid vtype = GetVtype(var->vartype);
		if(InvalidOid == vtype)
			return true;
		ctx->retType = vtype;
		if(ctx->replace)
			var->vartype = vtype;
		return false;
	}

	//Const treat as can be vectorzied, its return type is non-vectorized type
	//because we support the function like this: vtype op(vtype, const);
	if(IsA(node, Const))
	{
		Const *c = (Const*)node;
		ctx->retType = c->consttype;
		return false;
	}

	//OpExpr:args, return types should can be vectorized,
	//and there must exists an vectorized function to implement the operator
	if(IsA(node, OpExpr))
	{
		OpExpr *op = (OpExpr*)node;
		Node *argnode = NULL;
		Oid ltype, rtype, rettype;
		Form_pg_operator voper;
		HeapTuple tuple;

		//OpExpr mostly have two args, check the first one
		argnode = linitial(op->args);
		if(CheckVectorizedExpression(argnode, ctx))
			return true;

		ltype = ctx->retType;

		//check the second one
		argnode = lsecond(op->args);
		if(CheckVectorizedExpression(argnode, ctx))
			return true;

		rtype = ctx->retType;

		//check the return type
		rettype = GetVtype(op->opresulttype);
		if(InvalidOid == rettype)
			return true;


		//get the vectorized operator functions
		//NOTE:we have no ParseState now, Give the NULL value is OK but not good...
		tuple = oper(NULL, list_make1(makeString(get_opname(op->opno))),
			ltype, rtype, true, -1);
		if(NULL == tuple)
			return true;

		voper = (Form_pg_operator)GETSTRUCT(tuple);
		if(voper->oprresult != rettype)
		{
			//TODO
			//ReleaseOperator(tuple);
			ReleaseSysCache(tuple);
			return true;
		}

		if(ctx->replace)
		{
			op->opresulttype = rettype;
			op->opfuncid = voper->oprcode;
		}

		//ReleaseOperator(tuple);
		ReleaseSysCache(tuple);
		ctx->retType = rettype;
		return false;
	}

	/* support aggregate functions */
	/*
	if(IsA(node, Aggref))
	{
		Aggref *ref = (Aggref*)node;
		char *aggname = NULL;
		Oid retType;
		Oid vaggoid;

		if(ref->aggdistinct || NULL != ref->aggorder)
			return true;

		// Make sure there is less than one arguments
		if(1 < list_length(ref->args))
			return true;

		// check arguments
		if(NULL != ref->args)
		{
			if(CheckVectorizedExpression(linitial(ref->args), ctx))
				return true;
			retType = ctx->retType;
		}
		else
			retType = InvalidOid;

		// check the vectorized aggregate functions
		aggname = getProcNameOnlyFromOid(ref->aggfnoid);

		if(0 == strcmp(aggname, "count") &&
			0 == list_length(ref->args))
			aggname = "veccount";

		Assert(NULL != aggname);

		vaggoid = get_aggregate_oid(aggname, retType);
		if(InvalidOid == vaggoid)
			return true;

		if(ctx->replace)
			ref->aggfnoid = vaggoid;

		return false;
	}*/

	/*
	 * if there are const in expressions, it may need to convert
	 * type implicitly by FuncExpr, we only check if the arguments
	 * of the FuncExpr is constant.
	 */
	if(IsA(node, FuncExpr))
	{
		FuncExpr *f = (FuncExpr*)node;
//		ListCell *l = NULL;
		Node* expr = NULL;

		if(1 != list_length(f->args))
			return true;

		expr = (Node*)linitial(f->args);
		if(!IsA(expr, Const))
			return true;

		ctx->retType = f->funcresulttype;
		return false;
	}

	if(IsA(node,NullTest))
		return true;

	if(IsA(node, RowCompareExpr))
		return true;

	//now, other nodes treat as can not be vectorized
	return plan_tree_walker(node, CheckVectorizedExpression, ctx);;
}

/*
 * Replace the non-vectorirzed type to vectorized type
 */
static bool
ReplacePlanNodeWalker(PlannerInfo *root, Plan *plan)
{
	VectorizedContext ctx;

	planner_init_plan_tree_base(&ctx.base, root);

	ctx.replace = true;

	ctx.retType = InvalidOid;
	plan_tree_walker((Node*)plan,
					CheckVectorizedExpression,
					&ctx);


	return true;
}

/*
 * check the plan tree
 */
static Plan*
ReplacePlanVectorzied(PlannerInfo *root, Plan *plan)
{
	if(NULL == plan)
		return plan;

	ReplacePlanVectorzied(root, plan->lefttree);
	ReplacePlanVectorzied(root, plan->righttree);
	ReplacePlanNodeWalker(root, plan);

	return plan;
}

Plan*
CheckAndReplacePlanVectorized(PlannerInfo *root, Plan *plan)
{
	return ReplacePlanVectorzied(root, plan);
}

/*
 * map non-vectorized type to vectorized type.
 * To scan the PG_TYPE is inefficient, so we create a hashtable to map
 * the vectorized type and non-vectorized types.
 */
Oid GetVtype(Oid ntype)
{
	VecTypeHashEntry *entry = NULL;
	bool found = false;

	//construct the hash table
	if(NULL == hashMapN2V)
	{
		HASHCTL	hash_ctl;
		MemSet(&hash_ctl, 0, sizeof(hash_ctl));

		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(VecTypeHashEntry);
		hash_ctl.hash = oid_hash;

		hashMapN2V = hash_create("vectorized_v2n", 64/*enough?*/,
								&hash_ctl, HASH_ELEM | HASH_FUNCTION);

		/* insert int4->vint4 mapping manually, may construct from catalog in future */
		int t1=23;
		entry = hash_search(hashMapN2V, &t1, HASH_ENTER, &found);
		entry->dest = 18768;
	}

	//first, find the vectorized type in hash table
	entry = hash_search(hashMapN2V, &ntype, HASH_ENTER, &found);
	if(found)
		return entry->dest;
	/* currently only support int4 */
	return InvalidOid;
}

