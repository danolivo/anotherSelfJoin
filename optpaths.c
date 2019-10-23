#include "postgres.h"

#include "fmgr.h"
#include "nodes/pathnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "utils/guc.h"

#include "nodeSelfjoin.h"


PG_MODULE_MAGIC;

static set_join_pathlist_hook_type	prev_join_pathlist_hook = NULL;
//static create_upper_paths_hook_type	prev_create_upper_paths_hook = NULL;

/* GUC for debug and test purposes */
static bool remove_self_joins = true;
static int log_level;

/* GUC variables */
static const struct config_enum_entry format_options[] = {
	{"debug1", DEBUG1, false},
	{"info", INFO, false},
	{"log", LOG, false},
	{NULL, 0, false}
};

void _PG_init(void);
static bool walker(Node *node, void *context);
static void join_pathlist_hook(PlannerInfo *root, RelOptInfo *joinrel,
								RelOptInfo *outerrel, RelOptInfo *innerrel,
								JoinType jointype, JoinPathExtraData *extra);
static bool replace_outer_refs(Node *node, void *context);
//static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
//					RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);


void
_PG_init(void)
{
	DefineCustomBoolVariable("remove_self_joins",
							 "Turn on/off Self Joins removal",
							 NULL,
							 &remove_self_joins,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("log_level",
							 "Log level for Self Joins extension",
							 NULL,
							 &log_level,
							 DEBUG1,
							 format_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	SelfJoin_Init_methods();
	prev_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = join_pathlist_hook;
/*	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = create_upper_paths; */
}

typedef struct
{
	bool result;
	PlannerInfo *root;
} WalkerHook;

static bool
walker(Node *node, void *context)
{
	WalkerHook *data = (WalkerHook *) context;

	if (node == NULL)
		return false;

	if (IsA(node, OpExpr))
	{
		OpExpr *o = (OpExpr *) node;
		Var *lvar, *rvar;
		PlannerInfo *root = data->root;

		if (list_length(o->args) < 2 || !IsA((Node *) linitial(o->args), Var) ||
			!IsA((Node *) lsecond(o->args), Var))
			goto OnNegativeExit;

		Assert(list_length(o->args) == 2);

		lvar = (Var *) linitial(o->args);
		rvar = (Var *) lsecond(o->args);

		/* BASEREL type of RelOptInfo will guarantee of non-link value of varno, but */
		Assert(lvar->varno != INNER_VAR && lvar->varno != OUTER_VAR &&
													lvar->varno != INDEX_VAR);
		Assert(rvar->varno != INNER_VAR && rvar->varno != OUTER_VAR &&
													rvar->varno != INDEX_VAR);

		if (root->simple_rte_array[lvar->varno]->relid !=
			root->simple_rte_array[rvar->varno]->relid ||
			lvar->varattno != rvar->varattno)
			goto OnNegativeExit;

		/* Check uniqueness made before */
	}

	/* Should not find an unplanned subquery */
	Assert(!IsA(node, Query));

	return expression_tree_walker(node, walker, (void *) context);

OnNegativeExit:
	data->result = false;
	return false;
}

typedef struct
{
	Index oldvarno;
	Index newvarno;
} tlist_vars_replace;

static void
join_pathlist_hook(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
			RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	ListCell	*lc;
	WalkerHook data;
	List *childs = NIL;
	JoinPath *jp = NULL;
	tlist_vars_replace context;

	if (!remove_self_joins)
		return;

	data.result = true;
	data.root = root;

	if (prev_join_pathlist_hook)
		prev_join_pathlist_hook(root, joinrel, outerrel, innerrel, jointype, extra);

	if (innerrel->reloptkind != RELOPT_BASEREL ||
		outerrel->reloptkind != RELOPT_BASEREL)
	{
		elog(log_level, "Join can't be removed: inner or outer relation is not base relation: %d %d",
				innerrel->reloptkind, outerrel->reloptkind);
		return;
	}

	if (!extra->inner_unique)
	{
		elog(log_level, "Join can't be removed: inner relation is not unique");
		return;
	}

	/*
	 * Pass the join restrictions and check clauses for compliance with
	 * the self-join conditions.
	 */
	foreach(lc, extra->restrictlist)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		walker((Node *) rinfo->clause, (void *) &data);
	}
	if (!data.result)
	{
		elog(log_level, "Join can't be removed: clause can't pass verification");
		return;
	}

	/* Now self join type has proved */

	Assert(list_length(joinrel->pathlist) > 0);
	foreach(lc, joinrel->pathlist)
	{
		if (IsA(lfirst(lc), CustomPath))
			return;
	}

	/* XXX partial_pathlist */
	foreach(lc, joinrel->pathlist)
	{
		if (IsA(lfirst(lc), NestPath) || IsA(lfirst(lc), MergePath) ||
			IsA(lfirst(lc), HashPath))
		{
			jp = (JoinPath *) lfirst(lc);

			Assert(jp->innerjoinpath != NULL && jp->outerjoinpath != NULL);
			childs = lappend(childs, jp->outerjoinpath);
			childs = lappend(childs, jp->innerjoinpath);
			break;
		}

		continue;
	}

	/*
	 * Path list contains only self join scans.
	 */
	if (childs == NIL)
		return;

	context.oldvarno = innerrel->relid;
	context.newvarno = outerrel->relid;
	elog(log_level, "Replace varno %d with %d", context.oldvarno, context.newvarno);

	foreach(lc, innerrel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) copyObject(lfirst(lc));

		Assert(bms_is_member(context.oldvarno, rinfo->clause_relids));
		Assert(bms_is_empty(rinfo->nullable_relids));
		Assert(bms_is_empty(rinfo->outer_relids));

		Assert(bms_is_member(context.oldvarno, rinfo->left_relids));
		if (bms_is_member(context.oldvarno, rinfo->left_relids))
		{
			rinfo->left_relids = bms_del_member(rinfo->left_relids, context.oldvarno);
			rinfo->left_relids = bms_add_member(rinfo->left_relids, context.newvarno);
		}

		Assert(bms_is_empty(rinfo->right_relids));
		Assert(rinfo->orclause == NULL);

		bms_del_member(rinfo->clause_relids, context.oldvarno);
		bms_add_member(rinfo->clause_relids, context.newvarno);
		if (bms_is_member(context.oldvarno, rinfo->required_relids))
		{
			rinfo->required_relids = bms_del_member(rinfo->required_relids, context.oldvarno);
			rinfo->required_relids = bms_add_member(rinfo->required_relids, context.newvarno);
		}

		replace_outer_refs((Node *) rinfo->clause, &context);
		outerrel->baserestrictinfo = lappend(outerrel->baserestrictinfo, rinfo);
	}
/*
	foreach (lc, root->rowMarks)
	{
		PlanRowMark *rowMark = (PlanRowMark *) lfirst(lc);
		elog(INFO, "ROWMARK: rti=%d", rowMark->rti);
		if (rowMark->rti == context.oldvarno)
			rowMark->rti = context.newvarno;
	}
*/
	outerrel->reltarget->exprs = list_concat_copy(innerrel->reltarget->exprs, outerrel->reltarget->exprs);
	replace_outer_refs((Node *) joinrel->reltarget->exprs, &context);
	replace_outer_refs((Node *) outerrel->reltarget->exprs, &context);
	replace_outer_refs((Node *) root->processed_tlist, &context);

	add_path(joinrel, (Path *) create_sj_path(root, joinrel, childs));
}

static bool
replace_outer_refs(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var *var = (Var *) node;
		tlist_vars_replace *ctx = (tlist_vars_replace *) context;

		if (var->varno == ctx->oldvarno)
			var->varno = ctx->newvarno;
	}

	return expression_tree_walker(node, replace_outer_refs, context);
}
/*
#include "path_walker.h"

static bool fix_pathlist_replacements(Path *path, void *context)
{
	if (IsA(path, CustomPath))
	{
		Path *ipath, *opath;
		tlist_vars_replace context;

		Assert(((CustomPath *) path)->custom_paths != NIL);

		ipath = (Path *) linitial(((CustomPath *) path)->custom_paths);
		opath = (Path *) lsecond(((CustomPath *) path)->custom_paths);
		context.newvarno = ipath->parent->relid;
		context.oldvarno = opath->parent->relid;
		replace_outer_refs((Node *) path->pathtarget->exprs, &context);
	}

	return path_tree_walker(path, fix_pathlist_replacements, NULL);
}

static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	ListCell *lc;

	if (!remove_self_joins)
		return;

	foreach (lc, output_rel->pathlist)
	{
		Path *path = (Path *) lfirst(lc);

		fix_pathlist_replacements(path, NULL);
	}
} */
