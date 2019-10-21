/*-------------------------------------------------------------------------
 *
 * nodeSelfjoin.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "nodeSelfjoin.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"
#include "optimizer/cost.h"


static CustomPathMethods	SJ_Path_methods;
static CustomScanMethods	SJ_Plan_methods;
static CustomExecMethods	SJ_Exec_methods;


static struct Plan *SJCreateCustomPlan(PlannerInfo *root, RelOptInfo *rel,
									   struct CustomPath *best_path, List *tlist,
									   List *clauses, List *custom_plans);


/*
 * Create state of exchange node.
 */
static Node *
CreateSelfJoinState(CustomScan *node)
{
	CustomScanState	*state;

	state = (CustomScanState *) palloc0(sizeof(CustomScanState));
	NodeSetTag(state, T_CustomScanState);

	state->flags = node->flags;
	state->methods = &SJ_Exec_methods;
	state->custom_ps = NIL;

	return (Node *) state;
}

static void
BeginSelfJoin(CustomScanState *node, EState *estate, int eflags)
{
	CustomScan	*cscan = (CustomScan *) node->ss.ps.plan;
	Plan		*scan_plan;
	PlanState	*planState;

	/* At this moment we need only outer scan */
	Assert(list_length(cscan->custom_plans) == 1);
	scan_plan = (Plan *) linitial(cscan->custom_plans);
	planState = (PlanState *) ExecInitNode(scan_plan, estate, eflags);
	node->custom_ps = lappend(node->custom_ps, planState);

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitScanTupleSlot(estate, &node->ss, planState->ps_ResultTupleDesc,
															planState->scanops);
	ExecConditionalAssignProjectionInfo(&node->ss.ps,
									planState->ps_ResultTupleDesc, INDEX_VAR);
	return;
}

static TupleTableSlot *
ExecSelfJoin(CustomScanState *node)
{
	ScanState *subPlanState = linitial(node->custom_ps);
	ExprContext *econtext;

	econtext = node->ss.ps.ps_ExprContext;

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = ExecProcNode(&subPlanState->ps);

	if (TupIsNull(econtext->ecxt_scantuple))
	{
		if (node->ss.ps.ps_ProjInfo)
			return ExecClearTuple(node->ss.ps.ps_ProjInfo->pi_state.resultslot);
		else
			return NULL;
	}

	if (node->ss.ps.ps_ProjInfo)
		return ExecProject(node->ss.ps.ps_ProjInfo);
	else
		return econtext->ecxt_scantuple;
}

static void
ExecEndSelfJoin(CustomScanState *node)
{
	Assert(list_length(node->custom_ps) == 1);
	ExecEndNode(linitial(node->custom_ps));
}

static void
ExecReScanSelfJoin(CustomScanState *node)
{
	return;
}

static void
ExplainSelfJoin(CustomScanState *node, List *ancestors, ExplainState *es)
{
	StringInfoData		str;

	initStringInfo(&str);
	ExplainPropertyText("SelfJoin", str.data, es);
}

void
SelfJoin_Init_methods(void)
{
	SJ_Path_methods.CustomName = "PathSelfJoin";
	SJ_Path_methods.PlanCustomPath = SJCreateCustomPlan;
	SJ_Path_methods.ReparameterizeCustomPathByChild = NULL;

	SJ_Plan_methods.CustomName 				= "PlanSelfJoin";
	SJ_Plan_methods.CreateCustomScanState		= CreateSelfJoinState;
	RegisterCustomScanMethods(&SJ_Plan_methods);

	/* setup exec methods */
	SJ_Exec_methods.CustomName				= "ExecSelfJoin";
	SJ_Exec_methods.BeginCustomScan			= BeginSelfJoin;
	SJ_Exec_methods.ExecCustomScan			= ExecSelfJoin;
	SJ_Exec_methods.EndCustomScan				= ExecEndSelfJoin;
	SJ_Exec_methods.ReScanCustomScan			= ExecReScanSelfJoin;
	SJ_Exec_methods.MarkPosCustomScan			= NULL;
	SJ_Exec_methods.RestrPosCustomScan		= NULL;
	SJ_Exec_methods.EstimateDSMCustomScan  	= NULL;
	SJ_Exec_methods.InitializeDSMCustomScan 	= NULL;
	SJ_Exec_methods.InitializeWorkerCustomScan= NULL;
	SJ_Exec_methods.ReInitializeDSMCustomScan = NULL;
	SJ_Exec_methods.ShutdownCustomScan		= NULL;
	SJ_Exec_methods.ExplainCustomScan			= ExplainSelfJoin;
}

static CustomScan *
make_sj(List *custom_plans, List *tlist)
{
	CustomScan	*node = makeNode(CustomScan);
	Plan		*plan = &node->scan.plan;
	Plan		*subplan = linitial(custom_plans);

	plan->qual = NIL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->targetlist = tlist;

	/* Setup methods and child plan */
	node->methods = &SJ_Plan_methods;
	node->scan.scanrelid = 0;
	node->custom_plans = lappend(NIL, subplan);
	node->custom_scan_tlist = copyObject(subplan->targetlist);
	node->custom_exprs = NIL;
	node->custom_private = NIL;

	return node;
}

/*
 * Create and Initialize plan structure of Self Join node.
 */
static struct Plan *
SJCreateCustomPlan(PlannerInfo *root,
				   RelOptInfo *rel,
				   struct CustomPath *best_path,
				   List *tlist,
				   List *clauses,
				   List *custom_plans)
{
	CustomScan	*sj;

	sj = make_sj(custom_plans, tlist);

	sj->scan.plan.startup_cost = best_path->path.startup_cost;
	sj->scan.plan.total_cost = best_path->path.total_cost;
	sj->scan.plan.plan_rows = best_path->path.rows;
	sj->scan.plan.plan_width = best_path->path.pathtarget->width;
	sj->scan.plan.parallel_aware = best_path->path.parallel_aware;
	sj->scan.plan.parallel_safe = best_path->path.parallel_safe;

	return &sj->scan.plan;
}

#define cstmSubPath1(customPath) (Path *) linitial(((CustomPath *) \
													customPath)->custom_paths)

static void
cost_sj(PlannerInfo *root, RelOptInfo *rel, SJPath *sjp)
{
	Path *subpath;

	/* Estimate rel size as best we can with local statistics. */
	subpath = cstmSubPath1(sjp);
	sjp->cp.path.rows = subpath->rows;
	sjp->cp.path.startup_cost = subpath->startup_cost;
	sjp->cp.path.total_cost = subpath->total_cost;
}

/*
 * Can break the childrens list.
 */
SJPath *
create_sj_path(PlannerInfo *root, RelOptInfo *rel, List *childs)
{
	SJPath *sjp = ((SJPath *) newNode(sizeof(SJPath), T_CustomPath));
	Path *pathnode = &sjp->cp.path;

	pathnode->pathtype = T_CustomScan;
	pathnode->pathtarget = rel->reltarget;
	pathnode->param_info = NULL;
	pathnode->parent = rel;
	pathnode->parallel_aware = false; /* permanently */
	pathnode->parallel_safe = false; /* permanently */
	pathnode->parallel_workers = 0; /* permanently */
	pathnode->pathkeys = NIL;

	sjp->cp.flags = 0;
	sjp->cp.custom_paths = list_concat(sjp->cp.custom_paths, childs);

	sjp->cp.custom_private = NIL;
	sjp->cp.methods = &SJ_Path_methods;

	cost_sj(root, rel, sjp);
	return sjp;
}
