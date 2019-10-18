#include "postgres.h"

#include "fmgr.h"
#include "nodes/pathnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"

#include "nodeSelfjoin.h"


PG_MODULE_MAGIC;

static set_join_pathlist_hook_type	prev_join_pathlist_hook = NULL;
static create_upper_paths_hook_type	prev_create_upper_paths_hook = NULL;


void _PG_init(void);
static bool walker(Node *node, void *context);
static void join_pathlist_hook(PlannerInfo *root, RelOptInfo *joinrel,
								RelOptInfo *outerrel, RelOptInfo *innerrel,
								JoinType jointype, JoinPathExtraData *extra);
static bool replace_outer_refs(Node *node, void *context);
static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);


void
_PG_init(void)
{
	SelfJoin_Init_methods();
	prev_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = join_pathlist_hook;
	prev_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = create_upper_paths;
}

typedef struct
{
	bool result;
	PlannerInfo *root;
	RelOptInfo *rel;
	RelOptInfo *innerrel;
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

		elog(INFO, " --> %u %u <--",root->simple_rte_array[lvar->varno]->relid, root->simple_rte_array[rvar->varno]->relid);
		if (root->simple_rte_array[lvar->varno]->relid !=
			root->simple_rte_array[rvar->varno]->relid ||
			lvar->varattno != rvar->varattno)
			goto OnNegativeExit;

		/* Check uniqueness made before */
	}

	/* Should not find an unplanned subquery */
	Assert(!IsA(node, Query));

	return expression_tree_walker(node, walker,
								  (void *) context);
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

	data.result = true;
	data.root = root;
	data.rel = joinrel;
	data.innerrel = innerrel;

	if (prev_join_pathlist_hook)
		prev_join_pathlist_hook(root, joinrel, outerrel, innerrel, jointype, extra);

	if (innerrel->reloptkind != RELOPT_BASEREL ||
		outerrel->reloptkind != RELOPT_BASEREL)
		return;

	if (!extra->inner_unique)
		return;

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
		return;

	/* Now self join type has proved */

	Assert(list_length(joinrel->pathlist) > 0);
	foreach(lc, joinrel->pathlist)
	{
		if (IsA(lfirst(lc), CustomPath))
			return;
	}

	foreach(lc, joinrel->pathlist)
	{
		if (IsA(lfirst(lc), NestPath) || IsA(lfirst(lc), MergePath) ||
			IsA(lfirst(lc), HashPath))
		{
			jp = (JoinPath *) lfirst(lc);

			Assert(jp->innerjoinpath != NULL && jp->outerjoinpath != NULL);
			childs = lappend(childs, jp->innerjoinpath);
			childs = lappend(childs, jp->outerjoinpath);
			break;
		}

		continue;
	}

	/*
	 * Path list contains only self join scans.
	 */
	if (childs == NIL)
		return;

	context.oldvarno = outerrel->relid;
	context.newvarno = innerrel->relid;
	elog(INFO, "--> Replace %d with %d", context.oldvarno, context.newvarno);
	replace_outer_refs((Node *) joinrel->reltarget->exprs, &context);
	add_path(joinrel, (Path *) create_sj_path(root, joinrel, childs));
}

static bool
replace_outer_refs(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var *var = (Var *) node;
		tlist_vars_replace *ctx = (tlist_vars_replace *) context;

		if (var->varno == ctx->oldvarno)
			var->varno = ctx->newvarno;
	}

	return expression_tree_walker(node, replace_outer_refs, context);
}

static void create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	ListCell *lc;

	foreach (lc, output_rel->pathlist)
	{
		ProjectionPath *pj;
		Path *ipath, *opath;
		tlist_vars_replace context;

		if (!IsA((Node *) lfirst(lc), ProjectionPath))
			continue;

		pj = (ProjectionPath *) lfirst(lc);

		if (!IsA((Node *) pj->subpath, CustomPath))
			continue;

		Assert(((CustomPath *) pj->subpath)->custom_paths != NIL);
		opath = (Path *) lsecond(((CustomPath *) pj->subpath)->custom_paths);
		ipath = (Path *) linitial(((CustomPath *) pj->subpath)->custom_paths);
		context.oldvarno = opath->parent->relid;
		context.newvarno = ipath->parent->relid;
		replace_outer_refs((Node *) pj->path.pathtarget->exprs, &context);
	}
}
