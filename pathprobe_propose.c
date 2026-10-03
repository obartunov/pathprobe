/*-------------------------------------------------------------------------
 * pathprobe_propose.c
 * Part of pathprobe.  Distributed under the PostgreSQL License;
 * see the repository LICENSE file.
 *-------------------------------------------------------------------------
 */
/*
 * pathprobe_propose.c - proposed path injection for pathprobe v1.0
 *
 * Implements pathprobe_propose(query text, spec jsonb) RETURNS text.
 *
 * The spec is either a single candidate object (fields below) or a
 * multi-candidate form injecting several paths, possibly into different
 * relations, in one planning run:
 *   {"candidates": [ {spec}, {spec}, ... ]}
 * Candidate identity is the pathname (must be unique per call); probe1
 * events are labeled with it, so fate matching does not depend on costs.
 *
 * The proposed path is injected into the real planner search space of a
 * concrete query via set_rel_pathlist_hook.  It participates in ordinary
 * add_path() competition.  pathprobe shows its lifecycle.
 *
 * Lifecycle states a proposed path can reach:
 *   accepted   - entered the frontier
 *   displaced  - was on frontier, later pushed out by better path
 *   rejected   - entered add_path(), lost immediately
 *   survived   - still on rel->cheapest_total_path at rel hook time
 *
 * Spec format (JSON):
 * {
 *   "target":    {"relname": "orders"},
 *   "candidate": {"pathname": "duckdb_scan", "provider": "duckdb",
 *                 "pathtype": "CustomPath"},
 *   "cost":      {"startup_cost": 0.8, "total_cost": 18.0,
 *                 "rows": 1200, "width": 40},
 *   "planner":   {"parameterized": false, "parallel_safe": true,
 *                 "parallel_aware": false, "disabled_nodes": 0,
 *                 "pathkeys": ["col", ...]},   -- claim an output order:
 *                    matched as the longest prefix of the query's own
 *                    pathkeys (canonical ECs; no new PathKeys are built)
 *   "pushdown":  {"scan":true, "filter":true, "projection":true,
 *                 "aggregate":false, "order_by":false, "join":false,
 *                 "limit":false},
 *   "residual":  {"filter":false, "projection":false, "recheck":false},
 *   "mode":      {"winner_forbidden": false}
 * }
 *
 * winner_forbidden: if true, candidate gets disabled_nodes=1 so it
 * participates in competition but cannot become cheapest_total_path.
 * Useful for diagnostic / EXPLAIN-only experiments.
 *
 * Architecture:
 *   pathprobe_propose_internal() = pathprobe_internal() + proposal injected
 *   via set_rel_pathlist_hook.  A ProposedPathState is stored in a global
 *   (valid only during one planning call) so the hook can find the spec.
 *
 * Related layers (separate translation units):
 *   pathprobe_advisor.c - local single-query index-candidate generation for
 *     "advise":true; it feeds candidates into this mechanism through
 *     pathprobe_advise_generate().
 *   pathprobe_gate.c    - preflight query-load/risk boundary (scaffolding).
 * This file stays focused on explicit planner hypotheses: candidate/hypo
 * injection, the ACL boundary, planner interaction, event collection, and
 * fate/benefit reporting.
 */

#include "postgres.h"

#include <math.h>

#include "access/amapi.h"
#include "access/itup.h"
#include "catalog/pg_am_d.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "parser/parse_utilcmd.h"
#include "commands/defrem.h"
#include "utils/index_selfuncs.h"

#include "commands/explain.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parser.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "tcop/tcopprot.h"		/* pg_parse_query, pg_plan_queries */
#include "tcop/utility.h"

#include "pathprobe.h"
#include "pathprobe_security.h"
#include "pathprobe_propose.h"

static void retag_proposed_events(PathEventLog *log);


extern bool pathprobe_log_accepted; /* GUC from pathprobe.c */


static ProposedPathState * active_proposal = NULL;

/* ----------
 * CustomPathMethods for proposed paths
 *
 * winner_forbidden mode: PlanCustomPath returns an Error plan node with
 * a clear message.  For normal mode, we return a Result node (no-op
 * executor stand-in) - the goal of v0 is planner competition only.
 * ----------
 */
/*
 * Proposed paths are planner-observation tools, not execution paths.
 * Planning must always complete (the agent contract is "always a report"),
 * so PlanCustomPath builds a real CustomScan plan node; the guard sits at
 * the true execution boundary: CreateCustomScanState errors out.  A
 * planning-only probe never gets there; anything trying to actually run
 * the diagnostic plan does.
 */
static Node *
proposed_create_scan_state(struct CustomScan *cscan)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("pathprobe: proposed path \"%s\" (provider: %s) reached "
					"execution - this is a diagnostic-only candidate",
					(char *) linitial(cscan->custom_private),
					(char *) lsecond(cscan->custom_private)),
			 errhint("Proposed candidates exist for planner competition "
					 "only; the plan is not executable.")));
	return NULL;				/* unreachable */
}

static const CustomScanMethods proposed_scan_methods = {
	.CustomName = "ProposedPath",
	.CreateCustomScanState = proposed_create_scan_state,
};

static Plan *
proposed_plan_custom_path(PlannerInfo *root,
						  RelOptInfo *rel,
						  struct CustomPath *best_path,
						  List *tlist,
						  List *clauses,
						  List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.plan.startup_cost = best_path->path.startup_cost;
	cscan->scan.plan.total_cost = best_path->path.total_cost;
	cscan->scan.plan.plan_rows = best_path->path.rows;
	cscan->scan.plan.plan_width = best_path->path.pathtarget->width;
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_private = best_path->custom_private;
	cscan->methods = &proposed_scan_methods;
	return (Plan *) cscan;
}

static const CustomPathMethods proposed_path_methods = {
	.CustomName = "ProposedPath",
	.PlanCustomPath = proposed_plan_custom_path,
};

/*
 * pathprobe_proposed_pathname
 *
 * Return the candidate pathname if path is one of our injected proposed
 * paths, else NULL.  Used by probe1 hooks to label candidate events, so
 * fate matching does not depend on cost values.
 */
static const char *
hypo_name_for_ioi(IndexOptInfo *ioi)
{
	ListCell   *lc;

	if (!ioi || !ioi->hypothetical || active_proposal == NULL)
		return NULL;
	foreach(lc, active_proposal->hypo)
	{
		HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lc);

		if (h->ioi == ioi)
			return psprintf("hypo:%s", h->name);
	}
	return NULL;
}

const char *
pathprobe_proposed_pathname(const Path *path)
{
	if (!path)
		return NULL;

	if (IsA(path, CustomPath))
	{
		CustomPath *cpath = (CustomPath *) path;

		if (cpath->methods != &proposed_path_methods)
			return NULL;
		if (cpath->custom_private == NIL)
			return NULL;
		return (const char *) linitial(cpath->custom_private);
	}
	if (IsA(path, IndexPath))
		return hypo_name_for_ioi(((IndexPath *) path)->indexinfo);
	if (IsA(path, BitmapHeapPath))
	{
		Path	   *bq = ((BitmapHeapPath *) path)->bitmapqual;

		if (bq && IsA(bq, IndexPath))
			return hypo_name_for_ioi(((IndexPath *) bq)->indexinfo);
	}
	return NULL;
}


/* ----------
 * parse_spec_json
 *
 * Parse JSON spec into ProposedPathSpec.
 * Very simple recursive descent over the Jsonb structure.
 * Aborts with ereport(ERROR) on missing required fields.
 * ----------
 */
/*
 * spec_key - look up a string key in a JSONB object container.
 *
 * Wraps findJsonbValueFromContainer with the key setup so the two-level
 * descent and the typed getters do not each repeat it.
 */
static JsonbValue *
spec_key(JsonbContainer *c, const char *key)
{
	JsonbValue	k;

	if (c == NULL)
		return NULL;
	k.type = jbvString;
	k.val.string.val = unconstify(char *, key);
	k.val.string.len = strlen(key);
	return findJsonbValueFromContainer(c, JB_FOBJECT, &k);
}

/*
 * spec_path2 - descend spec.<path1>.<path2>; return the leaf value or NULL.
 * path1 must be an object; the caller checks the leaf jbv type.
 */
static JsonbValue *
spec_path2(Jsonb *jb, const char *path1, const char *path2)
{
	JsonbValue *jv;

	if (!JB_ROOT_IS_OBJECT(jb))
		return NULL;
	jv = spec_key(&jb->root, path1);
	if (!jv || jv->type != jbvBinary)
		return NULL;
	return spec_key((JsonbContainer *) jv->val.binary.data, path2);
}

static bool
json_get_bool(Jsonb *jb, const char *path1, const char *path2, bool defval)
{
	JsonbValue *jv = spec_path2(jb, path1, path2);

	return (jv && jv->type == jbvBool) ? jv->val.boolean : defval;
}

static double
json_get_number(Jsonb *jb, const char *path1, const char *path2, double defval)
{
	JsonbValue *jv = spec_path2(jb, path1, path2);

	if (jv && jv->type == jbvNumeric)
		return DatumGetFloat8(DirectFunctionCall1(numeric_float8,
												  NumericGetDatum(jv->val.numeric)));
	return defval;
}

static char *
json_get_string(Jsonb *jb, const char *path1, const char *path2,
				const char *defval)
{
	JsonbValue *jv = spec_path2(jb, path1, path2);

	if (!jv || jv->type != jbvString)
		return defval ? pstrdup(defval) : NULL;
	return pnstrdup(jv->val.string.val, jv->val.string.len);
}

static ProposedPathSpec *
parse_spec_json(Jsonb *spec_jb)
{
	ProposedPathSpec *spec = palloc0(sizeof(ProposedPathSpec));

	/* target */
	spec->relname = json_get_string(spec_jb, "target", "relname", NULL);
	if (!spec->relname)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("proposed path spec: target.relname is required")));

	/* candidate */
	spec->pathname = json_get_string(spec_jb, "candidate", "pathname", "proposed");
	spec->provider = json_get_string(spec_jb, "candidate", "provider", "unknown");

	/* cost */
	spec->startup_cost = (Cost) json_get_number(spec_jb, "cost", "startup_cost", 0.0);
	spec->total_cost = (Cost) json_get_number(spec_jb, "cost", "total_cost", 1.0);
	spec->rows = json_get_number(spec_jb, "cost", "rows", 1.0);
	spec->width = (int) json_get_number(spec_jb, "cost", "width", 8);

	/* planner */
	spec->parameterized = json_get_bool(spec_jb, "planner", "parameterized", false);
	spec->parallel_safe = json_get_bool(spec_jb, "planner", "parallel_safe", true);
	spec->parallel_aware = json_get_bool(spec_jb, "planner", "parallel_aware", false);
	spec->npathkeys = 0;
	{
		JsonbValue *pv = spec_key(&spec_jb->root, "planner");

		if (pv && pv->type == jbvBinary)
		{
			JsonbValue *av = spec_key((JsonbContainer *) pv->val.binary.data,
									  "pathkeys");

			if (av && av->type == jbvBinary &&
				JsonContainerIsArray(av->val.binary.data))
			{
				int			n = JsonContainerSize(av->val.binary.data);
				int			i;

				if (n > PATHPROBE_MAX_HYPO_COLS)
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									errmsg("pathprobe: planner.pathkeys: too many columns (max %d)",
										   PATHPROBE_MAX_HYPO_COLS)));
				for (i = 0; i < n; i++)
				{
					JsonbValue *sv = getIthJsonbValueFromContainer(av->val.binary.data, i);

					if (!sv || sv->type != jbvString)
						ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
										errmsg("pathprobe: planner.pathkeys[%d] is not a string", i)));
					spec->pathkey_cols[spec->npathkeys++] =
						pnstrdup(sv->val.string.val, sv->val.string.len);
				}
			}
		}
	}
	spec->disabled_nodes = (int) json_get_number(spec_jb, "planner", "disabled_nodes", 0);

	/* mode */
	spec->winner_forbidden = json_get_bool(spec_jb, "mode", "winner_forbidden", false);

	return spec;
}

/* ----------
 * propose_inject_hook
 *
 * set_rel_pathlist_hook adapter.
 * Called after normal paths are generated for each base rel.
 * If the rel matches target.relname, injects the proposed CustomPath
 * via add_path() - entering normal planner competition.
 * ----------
 */
static set_rel_pathlist_hook_type prev_propose_rel_hook = NULL;
#ifdef HAVE_PATHPROBE_INDEX_PATH_HOOK
static pathprobe_index_path_hook_type prev_index_path_hook = NULL;
#endif

/*
 * propose_match_pathkeys - canonical pathkeys for a candidate claim.
 * The spec lists output-order columns; we take the longest prefix of
 * root->query_pathkeys whose k-th pathkey's EquivalenceClass contains the
 * k-th claimed column of the target rel.  We never build new PathKeys:
 * add_path dominance (compare_pathkeys) works on canonical EC pointers,
 * and orders unrelated to the query are useless in that competition
 * anyway.  Direction comes from the query's own pathkey.
 */
static List *
propose_match_pathkeys(PlannerInfo *root, RelOptInfo *rel,
					   RangeTblEntry *rte, ProposedPathSpec * spec)
{
	List	   *result = NIL;
	ListCell   *lc;
	int			k = 0;

	if (spec->npathkeys == 0 || root->query_pathkeys == NIL)
		return NIL;
	foreach(lc, root->query_pathkeys)
	{
		PathKey    *pk = (PathKey *) lfirst(lc);
		AttrNumber	attno;
		ListCell   *lm;
		bool		found = false;

		if (k >= spec->npathkeys)
			break;
		attno = get_attnum(rte->relid, spec->pathkey_cols[k]);
		if (attno == InvalidAttrNumber || attno <= 0)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
							errmsg("pathprobe: planner.pathkeys: column \"%s\" not found in \"%s\"",
								   spec->pathkey_cols[k], spec->relname)));
		foreach(lm, pk->pk_eclass->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lm);
			Node	   *ex = (Node *) em->em_expr;

			if (em->em_is_const)
				continue;
			if (IsA(ex, RelabelType))
				ex = (Node *) ((RelabelType *) ex)->arg;
			if (IsA(ex, Var) && ((Var *) ex)->varno == rel->relid &&
				((Var *) ex)->varattno == attno)
			{
				found = true;
				break;
			}
		}
		if (!found)
			break;
		result = lappend(result, pk);
		k++;
	}
	return result;
}

static void
propose_inject_one(PlannerInfo *root, RelOptInfo *rel,
				   ProposedPathSpec * spec);


/*
 * Hints: constrain the counterfactual planning run via planner enable_* GUCs
 * ("what can be used / what must not").  Native planner semantics: forbidden
 * methods get disabled_nodes bumps, visible in events as disabled=N, so the
 * output shows *why* a path lost.  Scoped with GUC_ACTION_SAVE and rolled
 * back by AtEOXact_GUC(true, nestlevel) after planning (fmgr.c SET-clause
 * idiom), so the session state is untouched.
 */
static const char *const pathprobe_hint_methods[] = {
	"seqscan", "indexscan", "indexonlyscan", "bitmapscan", "tidscan",
	"sort", "incremental_sort", "hashagg", "material", "memoize",
	"nestloop", "mergejoin", "hashjoin", "gathermerge",
	"async_append", "parallel_append", "parallel_hash",
	"partition_pruning", "partitionwise_join", "partitionwise_aggregate",
	"distinct_reordering", "group_by_reordering", "eager_aggregate",
	"presorted_aggregate", "self_join_elimination",
	NULL
};

static void
propose_apply_hint_list(Jsonb *spec_jb, const char *key, const char *value)
{
	JsonbValue *arr;
	JsonbContainer *ac;
	int			n,
				i;

	arr = spec_key(&spec_jb->root, key);
	if (!arr)
		return;
	if (arr->type != jbvBinary || !JsonContainerIsArray(arr->val.binary.data))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("pathprobe: hints.%s must be an array of method names", key)));
	ac = arr->val.binary.data;
	n = JsonContainerSize(ac);
	for (i = 0; i < n; i++)
	{
		JsonbValue *mv = getIthJsonbValueFromContainer(ac, i);
		char	   *m;
		const char *const *p;
		char		guc[64];

		if (!mv || mv->type != jbvString)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: hints.%s[%d] is not a string", key, i)));
		m = pnstrdup(mv->val.string.val, mv->val.string.len);
		for (p = pathprobe_hint_methods; *p; p++)
			if (strcmp(m, *p) == 0)
				break;
		if (*p == NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: unknown method \"%s\" in hints.%s", m, key)));
		snprintf(guc, sizeof(guc), "enable_%s", m);
		(void) set_config_option(guc, value, PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, ERROR, false);
	}
}

/*
 * propose_apply_hints - returns a GUC nest level to be closed with
 * AtEOXact_GUC(true, level) after planning; 0 when no hints present.
 *
 * "allow_only" first disables every known method, then re-enables the
 * listed ones; "forbid" disables the listed ones.
 */
static int
propose_apply_hints(Jsonb *spec_jb)
{
	JsonbValue *hv;
	Jsonb	   *hints;
	int			nest;

	if (!JB_ROOT_IS_OBJECT(spec_jb))
		return 0;
	hv = spec_key(&spec_jb->root, "hints");
	if (!hv)
		return 0;
	if (hv->type != jbvBinary || !JsonContainerIsObject(hv->val.binary.data))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("pathprobe: \"hints\" must be an object")));
	hints = JsonbValueToJsonb(hv);

	nest = NewGUCNestLevel();

	if (spec_key(&hints->root, "allow_only"))
	{
		const char *const *p;
		char		guc[64];

		for (p = pathprobe_hint_methods; *p; p++)
		{
			snprintf(guc, sizeof(guc), "enable_%s", *p);
			(void) set_config_option(guc, "off", PGC_USERSET, PGC_S_SESSION,
									 GUC_ACTION_SAVE, true, ERROR, false);
		}
		propose_apply_hint_list(hints, "allow_only", "on");
	}
	propose_apply_hint_list(hints, "forbid", "off");
	return nest;
}

static bool
spec_get_bool(Jsonb *spec_jb, const char *key)
{
	JsonbValue *v;

	if (!JB_ROOT_IS_OBJECT(spec_jb))
		return false;
	v = spec_key(&spec_jb->root, key);
	return v && v->type == jbvBool && v->val.boolean;
}

static bool
spec_get_advise(Jsonb *spec_jb)
{
	return spec_get_bool(spec_jb, "advise");
}

/* true when the spec contains "candidates" or classic single-object keys */
static bool
spec_has_candidates(Jsonb *spec_jb)
{
	if (!JB_ROOT_IS_OBJECT(spec_jb))
		return true;
	if (spec_key(&spec_jb->root, "candidates"))
		return true;
	return spec_key(&spec_jb->root, "target") != NULL;
}

/*
 * pathprobe_analyze_one - parse-analyze and rewrite one raw statement.
 *
 * Single entry point for turning a RawStmt into a query tree, shared by the
 * baseline and injected probe paths so they cannot drift into different
 * analyze setups.
 */
static List *
pathprobe_analyze_one(RawStmt *parsetree, const char *query_string)
{
	Oid		   *ptypes = NULL;
	int			nparams = 0;

	return pg_analyze_and_rewrite_varparams(parsetree, query_string,
											&ptypes, &nparams, NULL);
}

/*
 * pathprobe_plan_one - plan one query tree list; return the leading
 * PlannedStmt, or NULL if planning produced none.  Both probe paths plan
 * through this call so the cursor options stay identical.
 */
static PlannedStmt *
pathprobe_plan_one(List *querytree_list, const char *query_string)
{
	List	   *plantree_list = pg_plan_queries(querytree_list, query_string,
											   CURSOR_OPT_PARALLEL_OK, NULL);

	return plantree_list != NIL ?
		linitial_node(PlannedStmt, plantree_list) : NULL;
}

/*
 * [B] query-level benefit baseline (pg_adviser totalCostSaved shape):
 * plan the same query once more with no injections and no logging (all our
 * hooks are gated on pathprobe_active_log / active_proposal, both NULL
 * here), and return the final plan total_cost.  The delta against the
 * injected run is the query-level benefit, which accounts for join effects
 * above the injected rel, unlike per-rel gaps.
 */
static double
propose_baseline_cost(const char *query_string)
{
	double		cost = -1.0;
	List	   *parsetree_list;
	ListCell   *lc;

	Assert(pathprobe_active_log == NULL && active_proposal == NULL);
	parsetree_list = pg_parse_query(query_string);
	foreach(lc, parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, lc);
		List	   *querytree_list = pathprobe_analyze_one(parsetree, query_string);
		PlannedStmt *ps = pathprobe_plan_one(querytree_list, query_string);

		if (ps && ps->planTree)
			cost = ps->planTree->total_cost;
	}
	return cost;
}

/*
 * [A] plan-walk attribution (pg_adviser mark_used_candidates shape):
 * a candidate that survived the frontier may still be absent from the final
 * plan; conversely "in plan" is the strongest signal.  Match our fake index
 * oids against IndexScan/IndexOnlyScan/BitmapIndexScan nodes.
 */
static void
propose_mark_in_plan(Plan *plan, List *hypo)
{
	Oid			indexid = InvalidOid;
	ListCell   *lc;

	if (plan == NULL)
		return;
	switch (nodeTag(plan))
	{
		case T_IndexScan:
			indexid = ((IndexScan *) plan)->indexid;
			break;
		case T_IndexOnlyScan:
			indexid = ((IndexOnlyScan *) plan)->indexid;
			break;
		case T_BitmapIndexScan:
			indexid = ((BitmapIndexScan *) plan)->indexid;
			break;
		default:
			break;
	}
	if (OidIsValid(indexid))
	{
		foreach(lc, hypo)
		{
			HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lc);

			if (h->ioi && h->ioi->indexoid == indexid)
				h->in_plan = true;
		}
	}
	propose_mark_in_plan(plan->lefttree, hypo);
	propose_mark_in_plan(plan->righttree, hypo);

	/* multi-child nodes are not covered by lefttree/righttree */
	switch (nodeTag(plan))
	{
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				propose_mark_in_plan((Plan *) lfirst(lc), hypo);
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
				propose_mark_in_plan((Plan *) lfirst(lc), hypo);
			break;
		case T_SubqueryScan:
			propose_mark_in_plan(((SubqueryScan *) plan)->subplan, hypo);
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScan *) plan)->custom_plans)
				propose_mark_in_plan((Plan *) lfirst(lc), hypo);
			break;
		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				propose_mark_in_plan((Plan *) lfirst(lc), hypo);
			break;
		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				propose_mark_in_plan((Plan *) lfirst(lc), hypo);
			break;
		default:
			break;
	}
}


static IndexOptInfo *hypo_build_from_stmt(PlannerInfo *root, RelOptInfo *rel,
										  RangeTblEntry *rte, HypoIndexSpec * h);

/* fake, catalog-free index oids for hypothetical entries */
static Oid	pathprobe_next_hypo_oid = 0x7FFF0000;

/*
 * parse_hypo_array - parse spec "hypo_indexes":[{relname,columns[,name]},...]
 */
static List *
parse_hypo_array(Jsonb *spec_jb)
{
	List	   *out = NIL;
	JsonbValue *arr;
	JsonbContainer *ac;
	int			n,
				i;

	arr = JB_ROOT_IS_OBJECT(spec_jb) ?
		spec_key(&spec_jb->root, "hypo_indexes") : NULL;
	if (!arr)
		return NIL;
	if (arr->type != jbvBinary || !JsonContainerIsArray(arr->val.binary.data))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("pathprobe: \"hypo_indexes\" must be an array")));
	ac = arr->val.binary.data;
	n = JsonContainerSize(ac);
	for (i = 0; i < n; i++)
	{
		JsonbValue *elem = getIthJsonbValueFromContainer(ac, i);
		Jsonb	   *ejb;
		HypoIndexSpec *h;
		JsonbValue *cols;
		JsonbContainer *cc;
		int			ncols,
					c;

		if (!elem || elem->type != jbvBinary)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: hypo_indexes[%d] is not an object", i)));
		ejb = JsonbValueToJsonb(elem);

		h = (HypoIndexSpec *) palloc0(sizeof(HypoIndexSpec));
		{
			JsonbValue *rv = spec_key(&ejb->root, "relname");

			if (!rv || rv->type != jbvString)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("pathprobe: hypo_indexes[%d]: \"relname\" is required", i)));
			h->relname = pnstrdup(rv->val.string.val, rv->val.string.len);

			rv = spec_key(&ejb->root, "name");
			if (rv && rv->type == jbvString)
				h->name = pnstrdup(rv->val.string.val, rv->val.string.len);
			else
				h->name = psprintf("hypo%d", i + 1);
		}

		cols = spec_key(&ejb->root, "columns");
		if (!cols || cols->type != jbvBinary ||
			!JsonContainerIsArray(cols->val.binary.data))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: hypo_indexes[%d]: \"columns\" array is required", i)));
		cc = cols->val.binary.data;
		ncols = JsonContainerSize(cc);
		if (ncols < 1 || ncols > PATHPROBE_MAX_HYPO_COLS)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: hypo_indexes[%d]: 1..%d columns supported",
								   i, PATHPROBE_MAX_HYPO_COLS)));
		h->ncols = ncols;
		for (c = 0; c < ncols; c++)
		{
			JsonbValue *cv = getIthJsonbValueFromContainer(cc, c);

			if (!cv || cv->type != jbvString)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("pathprobe: hypo_indexes[%d]: columns must be strings", i)));
			h->colnames[c] = pnstrdup(cv->val.string.val, cv->val.string.len);
		}
		out = lappend(out, h);
	}
	return out;
}

/*
 * parse_hypo_sql - "hypo_sql":["CREATE INDEX ... ON tbl (...)", ...]
 *
 * Full CREATE INDEX grammar via the real parser (HypoPG-style): raw_parser
 * -> IndexStmt -> transformIndexStmt.  Nothing is executed; the transformed
 * statement is only used to build a hypothetical IndexOptInfo.
 */
static List *
parse_hypo_sql(Jsonb *spec_jb)
{
	List	   *out = NIL;
	JsonbValue *arr;
	JsonbContainer *ac;
	int			n,
				i;

	arr = JB_ROOT_IS_OBJECT(spec_jb) ?
		spec_key(&spec_jb->root, "hypo_sql") : NULL;
	if (!arr)
		return NIL;
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pathprobe: hypo_sql is restricted to superusers in this release"),
				 errhint("Use structured hypo_indexes on relations you can select.")));
	if (arr->type != jbvBinary || !JsonContainerIsArray(arr->val.binary.data))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("pathprobe: \"hypo_sql\" must be an array of strings")));
	ac = arr->val.binary.data;
	n = JsonContainerSize(ac);
	if (n > pathprobe_max_hypo_indexes)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("pathprobe: hypo_sql: %d entries exceed pathprobe.max_hypo_indexes (%d)",
							   n, pathprobe_max_hypo_indexes)));
	for (i = 0; i < n; i++)
	{
		JsonbValue *sv = getIthJsonbValueFromContainer(ac, i);

		if (!sv || sv->type != jbvString)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: hypo_sql[%d] is not a string", i)));
		out = lappend(out,
					  hypo_spec_from_sql(pnstrdup(sv->val.string.val,
												  sv->val.string.len), i));
	}
	return out;
}

/*
 * hypo_spec_from_sql - one CREATE INDEX string -> HypoIndexSpec.
 * idx >= 0: user-facing hypo_sql[i] error wording; idx < 0: advisor use.
 */
HypoIndexSpec *
hypo_spec_from_sql(char *sql, int idx)
{
	List	   *ptree;
	IndexStmt  *stmt;
	Oid			relid;
	HypoIndexSpec *h;

	ptree = raw_parser(sql, RAW_PARSE_DEFAULT);
	if (list_length(ptree) != 1 ||
		!IsA(((RawStmt *) linitial(ptree))->stmt, IndexStmt))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("pathprobe: hypo_sql[%d] must be a single CREATE INDEX statement", idx)));
	stmt = (IndexStmt *) ((RawStmt *) linitial(ptree))->stmt;

	/* any AM with a routine is allowed; costs come from its amcostestimate
	 * (core guards hypothetical in gin/brin estimators; gist/spgist are
	 * generic).  Size estimate stays btree-shaped: physics = modeled. */
	if (stmt->unique || stmt->concurrent)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("pathprobe: hypo_sql[%d]: UNIQUE/CONCURRENTLY not supported for hypothetical indexes", idx)));

	relid = RangeVarGetRelid(stmt->relation, AccessShareLock, false);

	/* hypothetical probing exposes column names and cost/statistics */
	{
		AclResult	aclresult = pg_class_aclcheck(relid, GetUserId(),
												  ACL_SELECT);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLE,
						   stmt->relation->relname);
	}

	stmt = transformIndexStmt(relid, stmt, sql);

	h = (HypoIndexSpec *) palloc0(sizeof(HypoIndexSpec));
	h->amoid = get_index_am_oid(stmt->accessMethod ? stmt->accessMethod : "btree",
								false);
	h->stmt = stmt;
	h->sql = sql;
	h->relid = relid;
	h->relname = stmt->relation->relname;
	h->name = stmt->idxname ? stmt->idxname : psprintf("hyposql%d", idx + 1);
	return h;
}

/*
 * hypo_fill_am - fill AM capability fields from the real AM routine
 *
 * Mirrors plancat.c get_relation_info() (:308-347) so that hypothetical
 * indexes are costed by exactly the same amcostestimate and capability
 * flags the optimizer uses for real indexes of that AM.  btree is the
 * first consumer; enabling another AM later means lifting the parse guard
 * and providing a size model + canreturn policy for it.
 */
static void
hypo_fill_am(IndexOptInfo *ioi, Oid relam)
{
	const IndexAmRoutine *amroutine = GetIndexAmRoutineByAmId(relam, false);
	int			i;

	ioi->relam = relam;
	ioi->amcanorderbyop = amroutine->amcanorderbyop;
	ioi->amoptionalkey = amroutine->amoptionalkey;
	ioi->amsearcharray = amroutine->amsearcharray;
	ioi->amsearchnulls = amroutine->amsearchnulls;
	ioi->amcanparallel = amroutine->amcanparallel;
	ioi->amhasgettuple = (amroutine->amgettuple != NULL);
	/* heap always provides bitmap scan support */
	ioi->amhasgetbitmap = (amroutine->amgetbitmap != NULL);
	ioi->amcanmarkpos = (amroutine->ammarkpos != NULL &&
						 amroutine->amrestrpos != NULL);
	ioi->amcostestimate = amroutine->amcostestimate;
	Assert(ioi->amcostestimate != NULL);

	/* ordering: sortopfamily only for amcanorder AMs (plancat.c:333-347) */
	if (amroutine->amcanorder)
	{
		for (i = 0; i < ioi->nkeycolumns; i++)
			ioi->sortopfamily[i] = ioi->opfamily[i];
	}
	else
		ioi->sortopfamily = NULL;

	/*
	 * canreturn: the real path asks index_can_return() per column, which
	 * needs an index Relation a hypothetical index does not have.  btree's
	 * btcanreturn is unconditionally true; other AMs get false until a
	 * per-AM policy is added along with their size model.
	 */
	for (i = 0; i < ioi->ncolumns; i++)
		ioi->canreturn[i] = (relam == BTREE_AM_OID);
}

/*
 * hypo_build_indextlist - replicate plancat.c build_index_tlist (static
 * there) via syscache lookups; required for index-only scans and pathkeys.
 */
static List *
hypo_build_indextlist(RangeTblEntry *rte, IndexOptInfo *ioi)
{
	List	   *tlist = NIL;
	Index		varno = ioi->rel->relid;
	ListCell   *indexpr_item = list_head(ioi->indexprs);
	int			i;

	for (i = 0; i < ioi->ncolumns; i++)
	{
		int			indexkey = ioi->indexkeys[i];
		Expr	   *indexvar;

		if (indexkey != 0)
		{
			Oid			atttype = get_atttype(rte->relid, indexkey);
			int32		atttypmod;
			Oid			attcoll;

			get_atttypetypmodcoll(rte->relid, indexkey,
								  &atttype, &atttypmod, &attcoll);
			indexvar = (Expr *) makeVar(varno, indexkey,
										atttype, atttypmod, attcoll, 0);
		}
		else
		{
			if (indexpr_item == NULL)
				elog(ERROR, "pathprobe: wrong number of hypo index expressions");
			indexvar = (Expr *) lfirst(indexpr_item);
			indexpr_item = lnext(ioi->indexprs, indexpr_item);
		}
		tlist = lappend(tlist,
						makeTargetEntry(indexvar, i + 1, NULL, false));
	}
	return tlist;
}

/*
 * hypo_build_from_stmt - IndexOptInfo from a transformed CREATE INDEX
 *
 * Full grammar: expressions, partial WHERE, DESC/NULLS ordering, opclass,
 * INCLUDE.  Adapted from DefineIndex()/ComputeIndexAttrs() shape, following
 * HypoPG's hypo_index_store_parsetree.  btree only (guarded at parse).
 */
static IndexOptInfo *
hypo_build_from_stmt(PlannerInfo *root, RelOptInfo *rel,
					 RangeTblEntry *rte, HypoIndexSpec * h)
{
	IndexStmt  *stmt = h->stmt;
	IndexOptInfo *ioi = makeNode(IndexOptInfo);
	int			nkey = list_length(stmt->indexParams);
	int			ninc = list_length(stmt->indexIncludingParams);
	int			n = nkey + ninc;
	int			i;
	ListCell   *lc;
	double		key_width = 0.0;

	if (nkey < 1 || n > INDEX_MAX_KEYS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("pathprobe: hypo index \"%s\": bad column count", h->name)));

	ioi->indexoid = pathprobe_next_hypo_oid++;
	ioi->reltablespace = InvalidOid;
	ioi->rel = rel;
	ioi->ncolumns = n;
	ioi->nkeycolumns = nkey;
	ioi->indexkeys = (int *) palloc0(sizeof(int) * n);
	ioi->indexcollations = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->opfamily = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->opcintype = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->sortopfamily = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->reverse_sort = (bool *) palloc0(sizeof(bool) * nkey);
	ioi->nulls_first = (bool *) palloc0(sizeof(bool) * nkey);
	ioi->canreturn = (bool *) palloc0(sizeof(bool) * n);
	ioi->opclassoptions = (bytea **) palloc0(sizeof(bytea *) * n);
	ioi->indexprs = NIL;

	i = 0;
	foreach(lc, stmt->indexParams)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(lc);
		Oid			atttype;
		Oid			opclass;

		if (ielem->expr != NULL)
		{
			ioi->indexkeys[i] = 0;	/* expression column */
			ioi->indexprs = lappend(ioi->indexprs, copyObject(ielem->expr));
			atttype = exprType(ielem->expr);
			ioi->indexcollations[i] = exprCollation(ielem->expr);
		}
		else
		{
			AttrNumber	attno = get_attnum(rte->relid, ielem->name);

			if (attno == InvalidAttrNumber || attno <= 0)
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
								errmsg("pathprobe: hypo index \"%s\": column \"%s\" not found",
									   h->name, ielem->name)));
			ioi->indexkeys[i] = attno;
			atttype = get_atttype(rte->relid, attno);
			ioi->indexcollations[i] = get_typcollation(atttype);
		}

		opclass = ResolveOpClass(ielem->opclass, atttype,
								 get_am_name(h->amoid), h->amoid);
		ioi->opfamily[i] = get_opclass_family(opclass);
		ioi->opcintype[i] = get_opclass_input_type(opclass);

		ioi->reverse_sort[i] = (ielem->ordering == SORTBY_DESC);
		if (ielem->nulls_ordering == SORTBY_NULLS_DEFAULT)
			ioi->nulls_first[i] = ioi->reverse_sort[i];
		else
			ioi->nulls_first[i] = (ielem->nulls_ordering == SORTBY_NULLS_FIRST);

		key_width += get_typavgwidth(atttype, -1);
		i++;
	}
	foreach(lc, stmt->indexIncludingParams)
	{
		IndexElem  *ielem = (IndexElem *) lfirst(lc);
		AttrNumber	attno;

		if (ielem->expr != NULL)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("pathprobe: hypo index \"%s\": expressions in INCLUDE not supported",
								   h->name)));
		attno = get_attnum(rte->relid, ielem->name);
		if (attno == InvalidAttrNumber || attno <= 0)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
							errmsg("pathprobe: hypo index \"%s\": column \"%s\" not found",
								   h->name, ielem->name)));
		ioi->indexkeys[i] = attno;
		key_width += get_typavgwidth(get_atttype(rte->relid, attno), -1);
		i++;
	}

	/* partial index: predicate + predOK vs the rel's restrictions */
	if (stmt->whereClause != NULL)
	{
		ioi->indpred = make_ands_implicit((Expr *) copyObject(stmt->whereClause));
		ioi->predOK = predicate_implied_by(ioi->indpred,
										   rel->baserestrictinfo, false);
	}
	else
	{
		ioi->indpred = NIL;
		ioi->predOK = false;
	}

	/*
	 * check_index_predicates() already ran; keep the full baserestrictinfo
	 * (for predOK partial indexes this is slightly pessimistic costing: the
	 * implied quals are not removed).  Accepted narrow boundary.
	 */
	ioi->indrestrictinfo = rel->baserestrictinfo;
	ioi->indextlist = hypo_build_indextlist(rte, ioi);

	hypo_fill_am(ioi, h->amoid);
	ioi->unique = false;
	ioi->immediate = true;
	ioi->hypothetical = true;

	{
		int			entry_width = MAXALIGN(sizeof(IndexTupleData)) +
			MAXALIGN((int) key_width) + sizeof(ItemIdData);
		double		usable = (BLCKSZ - SizeOfPageHeaderData) * 0.90;
		double		epp = Max(1.0, usable / entry_width);

		ioi->tuples = rel->tuples;
		ioi->pages = (BlockNumber) Max(2, ceil(rel->tuples / epp));
	}
	ioi->tree_height = -1;

	return ioi;
}

/*
 * hypo_build_indexoptinfo - minimal hypothetical btree IndexOptInfo (v0)
 *
 * Plain heap columns only; default btree opclass per column.  pages is a
 * crude estimate (20 bytes/entry) - good enough for relative competition,
 * NOT a calibrated cost model.  check_index_predicates() already ran, so
 * indrestrictinfo must be filled here by hand.
 */
static IndexOptInfo *
hypo_build_indexoptinfo(PlannerInfo *root, RelOptInfo *rel,
						RangeTblEntry *rte, HypoIndexSpec * h)
{
	IndexOptInfo *ioi = makeNode(IndexOptInfo);
	int			n = h->ncols;
	int			nkey;
	int			c;

	if (h->stmt != NULL)
		return hypo_build_from_stmt(root, rel, rte, h);
	nkey = (h->nkeys > 0) ? h->nkeys : n;

	ioi->indexoid = pathprobe_next_hypo_oid++;
	ioi->reltablespace = InvalidOid;
	ioi->rel = rel;
	ioi->ncolumns = n;
	ioi->nkeycolumns = nkey;
	ioi->indexkeys = (int *) palloc0(sizeof(int) * n);
	ioi->indexcollations = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->opfamily = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->opcintype = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->sortopfamily = (Oid *) palloc0(sizeof(Oid) * nkey);
	ioi->reverse_sort = (bool *) palloc0(sizeof(bool) * nkey);
	ioi->nulls_first = (bool *) palloc0(sizeof(bool) * nkey);
	ioi->canreturn = (bool *) palloc0(sizeof(bool) * n);
	ioi->opclassoptions = (bytea **) palloc0(sizeof(bytea *) * n);

	for (c = 0; c < n; c++)
	{
		AttrNumber	attno = get_attnum(rte->relid, h->colnames[c]);
		Oid			atttype;
		Oid			opclass;

		if (attno == InvalidAttrNumber || attno <= 0)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
							errmsg("pathprobe: hypo index \"%s\": column \"%s\" not found in \"%s\"",
								   h->name, h->colnames[c], h->relname)));
		ioi->indexkeys[c] = attno;
		if (c >= nkey)
			continue;			/* INCLUDE column: no opclass machinery */
		atttype = get_atttype(rte->relid, attno);
		opclass = GetDefaultOpClass(atttype, BTREE_AM_OID);
		if (!OidIsValid(opclass))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
							errmsg("pathprobe: hypo index \"%s\": no default btree opclass for column \"%s\"",
								   h->name, h->colnames[c])));
		ioi->indexcollations[c] = get_typcollation(atttype);
		ioi->opfamily[c] = get_opclass_family(opclass);
		ioi->opcintype[c] = get_opclass_input_type(opclass);
	}

	hypo_fill_am(ioi, BTREE_AM_OID);
	ioi->indexprs = NIL;
	ioi->indpred = NIL;
	ioi->indrestrictinfo = rel->baserestrictinfo;	/* no predicate */
	ioi->indextlist = hypo_build_indextlist(rte, ioi);
	ioi->predOK = false;
	ioi->unique = false;
	ioi->immediate = true;
	ioi->hypothetical = true;

	/*
	 * Size estimate from actual key widths: btree leaf entry is the index
	 * tuple (header + MAXALIGNed keys) plus a line pointer, and leaf pages
	 * hold ~BTREE_DEFAULT_FILLFACTOR (90%) of usable space.  Same shape as
	 * HypoPG; an estimate, not a calibrated cost model.
	 */
	{
		int			entry_width = MAXALIGN(sizeof(IndexTupleData));
		double		usable;
		double		entries_per_page;

		for (c = 0; c < n; c++)
		{
			int32		w = get_typavgwidth(ioi->opcintype[c], -1);

			entry_width += MAXALIGN(w);
		}
		entry_width += sizeof(ItemIdData);
		usable = (BLCKSZ - SizeOfPageHeaderData) * 0.90;
		entries_per_page = Max(1.0, usable / entry_width);
		ioi->tuples = rel->tuples;
		ioi->pages = (BlockNumber) Max(2, ceil(rel->tuples / entries_per_page));
	}
	ioi->tree_height = -1;		/* let btcostestimate derive */

	return ioi;
}

/*
 * hypo_inject_one - offer hypothetical-index paths to the rel
 *
 * Temporarily replace rel->indexlist with the hypothetical entry alone and
 * rerun create_index_paths(): only new-index paths are generated, and they
 * compete through ordinary add_path() against the existing frontier.  The
 * entry is then appended to the restored indexlist for later planner phases.
 *
 * Injection runs from set_rel_pathlist_hook, not get_relation_info_hook as in
 * HypoPG: pathprobe needs the hypothetical index to enter real add_path()
 * competition under the active hooks so each resulting path is tagged in the
 * event log, which the earlier catalog-expansion stage cannot provide.
 */
static void
hypo_inject_one(PlannerInfo *root, RelOptInfo *rel,
				RangeTblEntry *rte, HypoIndexSpec * h)
{
	List	   *save = rel->indexlist;
	ListCell   *lc;

	h->ioi = hypo_build_indexoptinfo(root, rel, rte, h);
	h->rel_relids = pathprobe_active_log ?
		pelog_intern_relids(pathprobe_active_log, rel->relids) :
		bms_copy(rel->relids);
	/* create_index_paths() skips it at top level; OR arms may still use it */
	h->pred_not_implied = (h->ioi->indpred != NIL && !h->ioi->predOK);

	/*
	 * Swap in just the hypothetical index and regenerate index paths.  No
	 * PG_TRY guard is needed: if create_index_paths() throws, this RelOptInfo
	 * is discarded with the aborted planning, so the unrestored indexlist
	 * cannot outlive the error.
	 */
	rel->indexlist = list_make1(h->ioi);
	create_index_paths(root, rel);
	rel->indexlist = lappend(save, h->ioi);

	/* frontier check: label hypothetical-index paths that survived */
	if (pathprobe_active_log)
	{
		foreach(lc, rel->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);
			IndexOptInfo *pio = NULL;
			PathEvent	ev;

			if (IsA(p, IndexPath))
				pio = ((IndexPath *) p)->indexinfo;
			else if (IsA(p, BitmapHeapPath))
			{
				Path	   *bq = ((BitmapHeapPath *) p)->bitmapqual;

				if (bq && IsA(bq, IndexPath))
					pio = ((IndexPath *) bq)->indexinfo;
			}
			if (pio != h->ioi)
				continue;

			memset(&ev, 0, sizeof(ev));
			ev.relid = InvalidOid;
			ev.relids = pelog_intern_relids(pathprobe_active_log, rel->relids);
			ev.pathtype = p->pathtype;
			ev.kind = PEK_SURVIVED;
			ev.origin = PATH_ORIGIN_PROPOSED;
			ev.source = PATH_SOURCE_ADD_PATH;
			ev.is_parameterized = (p->param_info != NULL);
			ev.cost_startup = p->startup_cost;
			ev.cost_total = p->total_cost;
			ev.disabled_nodes = p->disabled_nodes;
			ev.label = MemoryContextStrdup(pathprobe_active_log->mcxt,
										   psprintf("hypo:%s", h->name));
			pelog_append(pathprobe_active_log, &ev);
		}
	}
}

static void
propose_inject_hook(PlannerInfo *root, RelOptInfo *rel,
					Index rti, RangeTblEntry *rte)
{
	ProposedPathState *state = active_proposal;
	char	   *relname;
	ListCell   *lc;

	/* Chain previous hook first */
	if (prev_propose_rel_hook)
		prev_propose_rel_hook(root, rel, rti, rte);

	if (!state)
		return;

	/* Only inject into base relations */
	if (rte->rtekind != RTE_RELATION)
		return;

	relname = get_rel_name(rte->relid);
	if (!relname)
		return;

	if (state->advise)
		pathprobe_advise_generate(root, rel, rte, state);

	foreach(lc, state->hypo)
	{
		HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lc);

		if (h->injected)
			continue;
		/* stmt form resolved a specific relation: match by oid, not name */
		if (OidIsValid(h->relid) ? (rte->relid != h->relid)
			: (strcmp(relname, h->relname) != 0))
			continue;

		hypo_inject_one(root, rel, rte, h);
		h->injected = true;
		state->injected = true;
	}

	foreach(lc, state->specs)
	{
		ProposedPathSpec *spec = (ProposedPathSpec *) lfirst(lc);

		if (spec->injected)
			continue;			/* inject each candidate only once */
		if (strcmp(relname, spec->relname) != 0)
			continue;

		propose_inject_one(root, rel, spec);
		spec->injected = true;
		state->injected = true;
	}
}

static void
propose_inject_one(PlannerInfo *root, RelOptInfo *rel,
				   ProposedPathSpec * spec)
{
	CustomPath *cpath;
	int			effective_disabled;

	/* Build the CustomPath */
	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.param_info = spec->parameterized ?
		get_baserel_parampathinfo(root, rel,
								  rel->lateral_relids)
		: NULL;
	cpath->path.parallel_aware = spec->parallel_aware;
	cpath->path.parallel_safe = spec->parallel_safe;
	cpath->path.parallel_workers = 0;
	cpath->path.rows = clamp_row_est(spec->rows);
	cpath->path.startup_cost = spec->startup_cost;
	cpath->path.total_cost = spec->total_cost;
	cpath->path.pathkeys = propose_match_pathkeys(root, rel,
												  planner_rt_fetch(rel->relid, root),
												  spec);

	/*
	 * disabled_nodes: spec->disabled_nodes = base value from spec
	 * winner_forbidden adds 1 more -> path competes but cannot win because
	 * set_cheapest sorts by disabled_nodes first.
	 */
	effective_disabled = spec->disabled_nodes +
		(spec->winner_forbidden ? 1 : 0);
	cpath->path.disabled_nodes = effective_disabled;

	cpath->flags = 0;
	cpath->custom_paths = NIL;

	/*
	 * Store pathname and provider in custom_private so PlanCustomPath can
	 * emit a useful error if execution is ever attempted.
	 */
	cpath->custom_private = list_make2(pstrdup(spec->pathname),
									   pstrdup(spec->provider));
	cpath->methods = &proposed_path_methods;

	/*
	 * Add to rel via normal add_path() - same competition as all other
	 * paths. probe1 hook will record the outcome
	 * (accepted/rejected/displaced).
	 */
	add_path(rel, (Path *) cpath);

	/*
	 * Frontier check at injection time: if the candidate is still in
	 * rel->pathlist after add_path competition, record a SURVIVED event so
	 * fate does not depend on probe0 ordering (probe0 snapshots the frontier
	 * before injection).  Later gather-stage displacement is not re-checked.
	 *
	 * Rejected paths are pfree'd inside add_path(); the membership guard is
	 * what makes the cpath dereferences below safe (no allocation happens
	 * between that pfree and this check).
	 */
	if (list_member_ptr(rel->pathlist, cpath) && pathprobe_active_log)
	{
		PathEvent	ev;

		memset(&ev, 0, sizeof(ev));
		ev.relid = InvalidOid;
		ev.relids = pelog_intern_relids(pathprobe_active_log, rel->relids);
		ev.pathtype = T_CustomScan;
		ev.kind = PEK_SURVIVED;
		ev.origin = PATH_ORIGIN_PROPOSED;
		ev.source = PATH_SOURCE_ADD_PATH;
		ev.cost_startup = cpath->path.startup_cost;
		ev.cost_total = cpath->path.total_cost;
		ev.disabled_nodes = cpath->path.disabled_nodes;
		ev.label = MemoryContextStrdup(pathprobe_active_log->mcxt,
									   spec->pathname);
		pelog_append(pathprobe_active_log, &ev);
	}
}

/* ----------
 * parse_spec_multi
 *
 * Accepts either the classic single-object spec, or a multi-candidate spec:
 *   {"candidates": [ {spec}, {spec}, ... ]}
 * Returns a List of ProposedPathSpec*.
 * ----------
 */
static List *
parse_spec_multi(Jsonb *spec_jb)
{
	List	   *specs = NIL;
	JsonbValue *arr;

	arr = JB_ROOT_IS_OBJECT(spec_jb) ?
		spec_key(&spec_jb->root, "candidates") : NULL;

	if (arr && arr->type == jbvBinary)
	{
		JsonbContainer *ac = arr->val.binary.data;
		int			n = JsonContainerSize(ac);
		int			i;

		if (!JsonContainerIsArray(ac))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: \"candidates\" must be an array")));
		for (i = 0; i < n; i++)
		{
			JsonbValue *elem = getIthJsonbValueFromContainer(ac, i);
			Jsonb	   *ejb;

			if (!elem || elem->type != jbvBinary)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("pathprobe: candidate %d is not an object", i)));
			ejb = JsonbValueToJsonb(elem);
			specs = lappend(specs, parse_spec_json(ejb));
		}
		if (specs == NIL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: \"candidates\" array is empty")));

		/* candidate identity is the pathname: require uniqueness */
		{
			ListCell   *a;

			foreach(a, specs)
			{
				ProposedPathSpec *sa = (ProposedPathSpec *) lfirst(a);
				ListCell   *b;

				for_each_cell(b, specs, lnext(specs, a))
				{
					ProposedPathSpec *sb = (ProposedPathSpec *) lfirst(b);

					if (strcmp(sa->pathname, sb->pathname) == 0)
						ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
										errmsg("pathprobe: duplicate candidate pathname \"%s\"",
											   sa->pathname)));
				}
			}
		}
		return specs;
	}

	/* classic single-object spec */
	return list_make1(parse_spec_json(spec_jb));
}

/* ----------
 * retag_proposed_events
 * ----------
 */
static void
retag_proposed_events(PathEventLog *log)
{
	int			i;

	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];

		if (ev->pathtype == T_CustomScan)
			ev->source = PATH_SOURCE_PROPOSED;
	}
}

/*  render_propose_text: removed, superseded by render_propose_card */

/* ----------
 * pathprobe_propose_internal
 *
 * Like pathprobe_internal() but with a proposed path injected.
 * Activates propose_inject_hook alongside normal pathprobe hooks.
 * ----------
 */
/*
 * Does any range table, at any query level, hold a plain relation with this
 * oid, or if the oid is invalid, this unqualified name?  Uses the same tests
 * as propose_inject_hook.  Inheritance and partition children are not in
 * the parse tree, so a target named by a child relation is reported as not
 * found.
 */
typedef struct TargetRel
{
	const char *relname;
	Oid			relid;
} TargetRel;

static bool
target_in_query_walker(Node *node, void *context)
{
	TargetRel  *target = (TargetRel *) context;

	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;
		char	   *name;

		if (rte->rtekind != RTE_RELATION)
			return false;
		if (OidIsValid(target->relid))
			return rte->relid == target->relid;
		name = get_rel_name(rte->relid);
		return name != NULL && strcmp(name, target->relname) == 0;
	}
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, target_in_query_walker,
								 context, QTW_EXAMINE_RTES_BEFORE);
	return expression_tree_walker(node, target_in_query_walker, context);
}

static bool
target_in_query(List *querytree_list, const char *relname, Oid relid)
{
	TargetRel	target = {relname, relid};

	return target_in_query_walker((Node *) querytree_list, &target);
}

#ifdef HAVE_PATHPROBE_INDEX_PATH_HOOK
/*
 * Record build_index_paths() decisions for the hypothetical indexes of the
 * active proposal.  Their IndexOptInfo is built by pathprobe, so pointer
 * identity is exact; real indexes are not recorded.
 */
static void
hypo_index_build_hook(PlannerInfo *root, const PathprobeIndexBuildInfo *info)
{
	ListCell   *lc;

	if (prev_index_path_hook)
		prev_index_path_hook(root, info);

	if (active_proposal == NULL || !info->index->hypothetical)
		return;

	foreach(lc, active_proposal->hypo)
	{
		HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lc);

		if (h->ioi != info->index)
			continue;

		switch (info->callsite)
		{
			case PATHPROBE_INDEX_CALL_PLAIN:
				h->build_plain++;
				break;
			case PATHPROBE_INDEX_CALL_SAOP_BITMAP:
				h->build_saop++;
				break;
			case PATHPROBE_INDEX_CALL_OR_ARM:
				h->build_or_arm++;
				break;
		}
		switch (info->outcome)
		{
			case PATHPROBE_INDEX_GENERATED:
				h->build_generated++;
				break;
			case PATHPROBE_INDEX_NO_USEFUL_INPUT:
				h->build_notgen |= HYPO_NOTGEN_NO_USEFUL_INPUT;
				break;
			case PATHPROBE_INDEX_NO_LEADING_CLAUSE:
				h->build_notgen |= HYPO_NOTGEN_NO_LEADING_CLAUSE;
				break;
			case PATHPROBE_INDEX_SCANTYPE_UNSUPPORTED:
				h->build_notgen |= HYPO_NOTGEN_SCANTYPE;
				break;
		}
		break;
	}
}
#endif

static PathProbeReport *
pathprobe_propose_internal(const char *query_string,
						   List *specs, List **hypo, bool advise,
						   bool advise_skip_prefix,
						   PathProbeOpts *opts,
						   PathProbeAccess *access,
						   Jsonb *spec_jb)
{
	MemoryContext log_mcxt;
	PathEventLog *log;
	PathProbeReport *report;
	List	   *parsetree_list;
	ListCell   *lc;
	ListCell   *lc2;
	double		plan_cost = -1.0;
	ProposedPathState state;
	int			hint_nest = 0;
	bool		hints_applied = false;

	log_mcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "PathProbeProposedLog",
									 ALLOCSET_DEFAULT_SIZES);
	log = pelog_create(log_mcxt);
#ifdef HAVE_PATHPROBE_INDEX_PATH_HOOK
	log->coverage |= PP_COV_INDEX_BUILD;
#endif

	/* Activate pathprobe hooks */
	pathprobe_active_log = log;
	pathprobe_active_opts = opts;

	/* Activate proposal state */
	state.specs = specs;
	state.hypo = *hypo;
	state.advise = advise;
	state.advise_skip_prefix = advise_skip_prefix;
	state.atoms_str = NULL;
	state.advise_generated = 0;
	state.advise_truncated = false;
	state.root = NULL;
	state.injected = false;
	state.winner = false;
	state.access = access;

	/*
	 * Single active probe at a time: active_proposal and the saved pathlist
	 * hook are process-global, so pathprobe_propose_internal is not reentrant.
	 * Planning does not re-enter a pathprobe SQL function, so this holds; the
	 * assert catches a future path that would nest one probe inside another.
	 */
	Assert(active_proposal == NULL);
	active_proposal = &state;

	/* Install proposal inject hook (chains probe0) */
	prev_propose_rel_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = propose_inject_hook;
#ifdef HAVE_PATHPROBE_INDEX_PATH_HOOK
	prev_index_path_hook = pathprobe_index_path_hook;
	pathprobe_index_path_hook = hypo_index_build_hook;
#endif

	PG_TRY();
	{
		parsetree_list = pg_parse_query(query_string);

		/*
		 * Non-superuser probes are a single statement.  Enforce this on the
		 * raw parsetree list before the loop: the per-statement authorize
		 * check below only sees one statement's query tree and cannot tell
		 * that several statements were submitted, so a multi-statement string
		 * would otherwise slip through one statement at a time.
		 */
		if (!superuser() && list_length(parsetree_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pathprobe: non-superuser probes must be a single statement")));

		foreach(lc, parsetree_list)
		{
			RawStmt    *parsetree = lfirst_node(RawStmt, lc);
			List	   *querytree_list;
			PlannedStmt *ps;
			ListCell   *sc;

			querytree_list = pathprobe_analyze_one(parsetree, query_string);

			/* ACL gate: authorize before any planning happens */
			pathprobe_authorize_query(querytree_list, access);

			/* target identity: tells not_examined from target_not_found */
			foreach(sc, specs)
			{
				ProposedPathSpec *spec = (ProposedPathSpec *) lfirst(sc);

				if (!spec->target_in_query)
					spec->target_in_query =
						target_in_query(querytree_list, spec->relname,
										InvalidOid);
			}
			foreach(sc, *hypo)
			{
				HypoIndexSpec *h = (HypoIndexSpec *) lfirst(sc);

				if (!h->target_in_query)
					h->target_in_query =
						target_in_query(querytree_list, h->relname, h->relid);
			}

			/* structured hypo indexes must target vetted relations */
			if (!access->is_superuser)
			{
				ListCell   *hc;

				foreach(hc, *hypo)
				{
					HypoIndexSpec *h = (HypoIndexSpec *) lfirst(hc);
					Oid			hrelid = h->relid;

					/* stmt form carries relid; structured form has relname */
					if (!OidIsValid(hrelid) && h->relname)
					{
						RangeVar   *rv = makeRangeVar(NULL, h->relname, -1);

						hrelid = RangeVarGetRelid(rv, AccessShareLock, true);
					}
					pathprobe_check_hypo_relation(access, hrelid, h->amoid,
												  h->stmt != NULL);

					/*
					 * Structured hypo: the caller must be able to SELECT every
					 * named column, not just the relation.  (stmt/hypo_sql form
					 * is already superuser-only, rejected above.)
					 */
					if (h->stmt == NULL)
						pathprobe_check_hypo_columns(access, hrelid,
													 h->colnames, h->ncols);
				}
			}

			/*
			 * Apply planner hints only AFTER authorization and the hypo
			 * checks have passed for this (single, for non-superusers)
			 * statement: hints change enable_* GUCs, and no GUC-changing
			 * operation may run before the ACL gate.  Applied once; the nest
			 * level is closed on every exit path below.
			 */
			if (!hints_applied)
			{
				hint_nest = propose_apply_hints(spec_jb);
				hints_applied = true;
			}

			if (querytree_list != NIL)
			{
				Query	   *q = linitial_node(Query, querytree_list);

				if (q->rtable)
					pelog_build_rtmap(log, q->rtable);
			}

			ps = pathprobe_plan_one(querytree_list, query_string);
			if (ps && ps->planTree)
			{
				plan_cost = ps->planTree->total_cost;
				propose_mark_in_plan(ps->planTree, state.hypo);
				foreach(lc2, ps->subplans)
					propose_mark_in_plan((Plan *) lfirst(lc2), state.hypo);
			}
		}
	}
	PG_CATCH();
	{
		if (hint_nest > 0)
			AtEOXact_GUC(true, hint_nest);
		set_rel_pathlist_hook = prev_propose_rel_hook;
#ifdef HAVE_PATHPROBE_INDEX_PATH_HOOK
		pathprobe_index_path_hook = prev_index_path_hook;
#endif
		active_proposal = NULL;
		pathprobe_active_log = NULL;
		pathprobe_active_opts = NULL;
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Restore hooks */
	set_rel_pathlist_hook = prev_propose_rel_hook;
#ifdef HAVE_PATHPROBE_INDEX_PATH_HOOK
	pathprobe_index_path_hook = prev_index_path_hook;
#endif
	active_proposal = NULL;
	*hypo = state.hypo;			/* advise mode may have generated entries */
	pathprobe_active_log = NULL;
	pathprobe_active_opts = NULL;

	report = MemoryContextAlloc(log_mcxt, sizeof(PathProbeReport));
	report->log = log;
	report->injected = state.injected;
	report->plan_cost = plan_cost;
	report->baseline_cost = -1.0;
	report->advise_atoms = state.atoms_str;
	report->advisor_truncated = state.advise_truncated;

	/*
	 * Output guard (defence in depth): every hypothetical index relation that
	 * reached the report must be in the authorized set.  Events and advisor
	 * candidates derive from the vetted query range table and hypo list, so
	 * this closes the loop without a parallel relid walk.  On violation we
	 * ERROR rather than redact.
	 */
	if (!access->is_superuser)
	{
		ListCell   *hc;

		foreach(hc, *hypo)
		{
			HypoIndexSpec *h = (HypoIndexSpec *) lfirst(hc);

			if (OidIsValid(h->relid))
				pathprobe_assert_relation_allowed(access, h->relid);
		}
	}

	/*
	 * Baseline cost (benefit denominator) is planned with the injection hooks
	 * already restored but the hints still active, matching the conditions of
	 * the counterfactual run.  Ownership of the hint GUC nest level stays
	 * entirely inside this function: close it on every exit path, including if
	 * baseline planning throws.
	 */
	if (hint_nest > 0)
	{
		PG_TRY();
		{
			if (*hypo != NIL || advise)
				report->baseline_cost = propose_baseline_cost(query_string);
		}
		PG_CATCH();
		{
			AtEOXact_GUC(true, hint_nest);
			PG_RE_THROW();
		}
		PG_END_TRY();
		AtEOXact_GUC(true, hint_nest);
	}
	else if (*hypo != NIL || advise)
		report->baseline_cost = propose_baseline_cost(query_string);

	return report;
}

/*
 * Hypothetical index fate.
 *
 * One hypo index yields several paths (plain, parameterized, bitmap, both
 * scan directions), so its fate aggregates the labeled add_path() events of
 * all of them at the index's relation, after the generation decision
 * recorded by the index path hook.  As for candidates, a missing event
 * moves the state only when the point that would report it is covered;
 * otherwise the result is unknown.
 */
typedef struct HypoCounts
{
	int			accepted;
	int			rejected;
	int			survived;
	int			displaced;
	bool		best_set;
	Cost		best;
} HypoCounts;

typedef enum HypoFate
{
	HYPO_TARGET_NOT_FOUND,		/* no such relation in the query */
	HYPO_NOT_EXAMINED,			/* build_index_paths() never called for it */
	HYPO_NOT_GENERATED,			/* called, no path built */
	HYPO_REJECTED,				/* every observed add_path() rejected it */
	HYPO_DISPLACED,				/* accepted, all accepted paths displaced */
	HYPO_SURVIVED_AT_POINT,		/* in the pathlist after injection;
								 * acceptance/displacement not covered */
	HYPO_SURVIVED,				/* accepted paths not all displaced */
	HYPO_UNKNOWN,
	HYPO_ILLEGAL				/* contradicts a covered decision point */
} HypoFate;

/*
 * Labeled events of h's paths.  With base_only, only events at the rel h
 * was injected into, matched by planner relids: an index path carried into
 * an upper rel unchanged is labeled too, and is not counted then.
 */
static void
hypo_count_events(PathEventLog *log, HypoIndexSpec * h, HypoCounts * c,
				  bool base_only)
{
	char	   *tag = psprintf("hypo:%s", h->name);
	int			i;

	memset(c, 0, sizeof(*c));
	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];

		if (!ev->label || strcmp(ev->label, tag) != 0)
			continue;
		if (base_only &&
			(h->rel_relids == NULL || !bms_equal(ev->relids, h->rel_relids)))
			continue;
		if (ev->kind == PEK_ACCEPTED)
			c->accepted++;
		else if (ev->kind == PEK_REJECTED)
			c->rejected++;
		else if (ev->kind == PEK_SURVIVED)
			c->survived++;
		else if (ev->kind == PEK_DISPLACED)
			c->displaced++;
		if (!c->best_set || ev->cost_total < c->best)
		{
			c->best = ev->cost_total;
			c->best_set = true;
		}
	}
	pfree(tag);
}

/*
 * *reason is set for NOT_EXAMINED and UNKNOWN; NOT_GENERATED reasons are
 * the HYPO_NOTGEN_* bits in h->build_notgen.
 */
static HypoFate
hypo_fate(PathEventLog *log, HypoIndexSpec * h, const char **reason)
{
	uint32		acc_disp = PP_COV_ACCEPTED | PP_COV_DISPLACED;
	HypoCounts	base;
	HypoCounts *c = &base;
	bool		observed;

	hypo_count_events(log, h, c, true);
	observed = (c->accepted + c->rejected + c->survived + c->displaced) > 0;
	*reason = NULL;
	if (!h->injected)
	{
		if (!h->target_in_query)
			return HYPO_TARGET_NOT_FOUND;
		*reason = "rel_not_planned";
		return HYPO_NOT_EXAMINED;
	}

	if (log->coverage & PP_COV_INDEX_BUILD)
	{
		int			calls = h->build_plain + h->build_saop + h->build_or_arm;

		if (calls == 0)
		{
			/* the only skip before the call that applies to a hypo index */
			if (observed || !h->pred_not_implied)
				return HYPO_ILLEGAL;
			*reason = "predicate_not_implied";
			return HYPO_NOT_EXAMINED;
		}
		if (h->build_generated == 0)
			return observed ? HYPO_ILLEGAL : HYPO_NOT_GENERATED;
	}
	else if (!observed)
	{
		/* an OR arm may still have examined a partial index */
		*reason = h->pred_not_implied ? "predicate_not_implied" :
			"index_build_not_covered";
		return HYPO_UNKNOWN;
	}

	/* paths were generated; what happened to them in add_path() */
	if ((log->coverage & acc_disp) == acc_disp)
	{
		if (c->displaced > c->accepted ||
			(c->survived > 0 && c->accepted == 0))
			return HYPO_ILLEGAL;
		if (c->accepted > c->displaced)
			return HYPO_SURVIVED;
		if (c->accepted > 0)
			return HYPO_DISPLACED;
		if (c->rejected > 0)
			return HYPO_REJECTED;
	}
	else if (c->survived > 0)
		return HYPO_SURVIVED_AT_POINT;

	/* e.g. only inside a BitmapAnd/BitmapOr, which carries no label */
	*reason = "outcome_not_observed";
	return HYPO_UNKNOWN;
}

static const char *
hypo_fate_text(HypoFate f)
{
	switch (f)
	{
		case HYPO_TARGET_NOT_FOUND:
			return "target_not_found";
		case HYPO_NOT_EXAMINED:
			return "not_examined";
		case HYPO_NOT_GENERATED:
			return "not_generated";
		case HYPO_REJECTED:
			return "rejected";
		case HYPO_DISPLACED:
			return "displaced";
		case HYPO_SURVIVED_AT_POINT:
			return "survived_at_point";
		case HYPO_SURVIVED:
			return "survived";
		case HYPO_ILLEGAL:
			return "inconsistent";
		case HYPO_UNKNOWN:
		default:
			return "unknown";
	}
}

static void
hypo_notgen_append(StringInfo out, uint32 bits, bool json_form)
{
	static const struct
	{
		uint32		bit;
		const char *name;
	}			names[] = {
		{HYPO_NOTGEN_NO_USEFUL_INPUT, "no_useful_input"},
		{HYPO_NOTGEN_NO_LEADING_CLAUSE, "no_leading_clause"},
		{HYPO_NOTGEN_SCANTYPE, "scantype_unsupported"},
	};
	bool		first = true;

	for (int i = 0; i < lengthof(names); i++)
	{
		if (!(bits & names[i].bit))
			continue;
		if (json_form)
			appendStringInfo(out, "%s\"%s\"", first ? "" : ",", names[i].name);
		else
			appendStringInfo(out, "%s%s", first ? "" : ",", names[i].name);
		first = false;
	}
}

/* ----------
 * SQL wrapper: pathprobe_propose(query text, spec text) RETURNS text
 *
 * spec is JSON text (not jsonb) for simplicity - avoids pg_catalog lookup.
 * ----------
 */
static void render_propose_card(PathProbeReport *, StringInfo, PathProbeOpts *, ProposedPathSpec *, bool, bool);



static void
pathprobe_propose_register_label_provider(void)
{
	/* backend-local, idempotent; resolver returns NULL for ordinary paths */
	pathprobe_set_path_label_provider(pathprobe_proposed_pathname);
}

PG_FUNCTION_INFO_V1(pathprobe_propose);

Datum
pathprobe_propose(PG_FUNCTION_ARGS)
{
	text	   *query_text = PG_GETARG_TEXT_PP(0);
	text	   *spec_text = PG_GETARG_TEXT_PP(1);
	char	   *query_str = text_to_cstring(query_text);

	char	   *spec_str = text_to_cstring(spec_text);
	Datum		spec_datum;
	Jsonb	   *spec_jb;
	List	   *specs;
	List	   *hypo;
	bool		advise;
	ListCell   *lc;
	PathProbeOpts opts = {false, false};
	PathProbeAccess access;
	PathProbeReport *report;
	StringInfoData out;

	/* Parse spec JSON -> Jsonb */
	if (strlen(query_str) > (size_t) pathprobe_max_query_length)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("pathprobe: query longer than pathprobe.max_query_length (%d)",
							   pathprobe_max_query_length)));

	pathprobe_propose_register_label_provider();

	spec_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(spec_str));
	spec_jb = DatumGetJsonbP(spec_datum);
	advise = spec_get_advise(spec_jb);
	hypo = list_concat(parse_hypo_array(spec_jb), parse_hypo_sql(spec_jb));
	specs = ((hypo != NIL || advise) && !spec_has_candidates(spec_jb)) ? NIL
		: parse_spec_multi(spec_jb);

	/* Force log_accepted=true so proposed path accepted events are visible */
	{
		bool		saved_log_accepted = pathprobe_log_accepted;

		pathprobe_log_accepted = true;
		/* hints + baseline are applied inside, only after authorization */
		report = pathprobe_propose_internal(query_str, specs, &hypo, advise,
											spec_get_bool(spec_jb, "advise_skip_prefix"),
											&opts, &access, spec_jb);
		pathprobe_log_accepted = saved_log_accepted;
	}

	initStringInfo(&out);
	foreach(lc, specs)
	{
		ProposedPathSpec *spec = (ProposedPathSpec *) lfirst(lc);

		if (out.len > 0)
			appendStringInfoString(&out, "\n");
		render_propose_card(report, &out, &opts, spec, spec->injected,
							hypo == NIL && lc == list_tail(specs));
	}

	if (hypo != NIL)
	{
		if (out.len > 0)
			appendStringInfoString(&out, "\n");
		appendStringInfoString(&out,
							   "HYPOTHETICAL INDEXES\n"
							   "----------------------------------------------------------------------\n");
		if (report->advise_atoms && report->advise_atoms[0])
			appendStringInfo(&out, "atoms  : %s   (=eq, ~range)\n",
							 report->advise_atoms);
		appendStringInfoString(&out,
							   "physics: modeled (costs: am-native estimator; size: btree-shaped)\n");
		foreach(lc, hypo)
		{
			HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lc);

			if (h->sql)
				appendStringInfo(&out, "hypo:%-12s [%s", h->name, h->sql);
			else
			{
				appendStringInfo(&out, "hypo:%-12s on %s(", h->name, h->relname);
				for (int c = 0; c < h->ncols; c++)
					appendStringInfo(&out, "%s%s", c ? "," : "", h->colnames[c]);
			}
			{
				const char *closer = h->sql ? "]" : ")";
				HypoCounts	hc;
				HypoFate	hf;
				const char *reason;

				hypo_count_events(report->log, h, &hc, false);
				hf = hypo_fate(report->log, h, &reason);
				if (!h->injected)
					appendStringInfo(&out, "%s  not injected", closer);
				else
				{
					appendStringInfo(&out, "%s  injected: acc=%d rej=%d surv=%d",
									 closer, hc.accepted, hc.rejected,
									 hc.survived);
					if (hc.best_set)
						appendStringInfo(&out, "  best=%.2f", hc.best);
					if (h->in_plan)
						appendStringInfoString(&out, "  IN PLAN");
				}
				appendStringInfo(&out, "  fate=%s", hypo_fate_text(hf));
				if (reason)
					appendStringInfo(&out, "(%s)", reason);
				else if (hf == HYPO_NOT_GENERATED)
				{
					appendStringInfoChar(&out, '(');
					hypo_notgen_append(&out, h->build_notgen, false);
					appendStringInfoChar(&out, ')');
				}
				appendStringInfoChar(&out, '\n');
			}
		}
		if (report->baseline_cost >= 0 && report->plan_cost >= 0)
			appendStringInfo(&out,
							 "plan cost: %.2f -> %.2f  (query-level benefit %+.2f)\n",
							 report->baseline_cost, report->plan_cost,
							 report->baseline_cost - report->plan_cost);
		appendStringInfoChar(&out, '\n');
		/* full event view: hypo paths carry "hypo:<name>" labels */
		render_text(report, &out, &opts);
	}

	/* never return an empty string: advise with nothing to advise still
	 * has an observable planning run (agent contract: always a report) */
	if (out.len == 0)
		render_text(report, &out, &opts);

	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/*
 * Candidate fate, derived from the labeled event stream (seq order) and the
 * log's coverage.  This applies to CustomPath candidates only: one path per
 * identity.  Hypothetical indexes keep aggregate counters - many paths
 * share one hypo label, a per-identity model is the wrong one there.
 *
 * Only observed events move the state.  When the decision point that would
 * report an outcome is not covered in this run, the result is one of the
 * unknown states instead of an outcome inferred from a missing event.
 *
 * At the target rel the candidate is rejected, or accepted; once accepted
 * it is either seen in the pathlist right after its own add_path()
 * (SURVIVED, which implies acceptance) and possibly displaced later.  An
 * acceptance into an upper rel means the candidate path itself was carried
 * up; rejection or displacement in an upper rel does not undo that.
 * Sequences that contradict a covered decision point are reported as
 * illegal: either the planner did something we do not model, or a probe
 * is wrong.
 */
typedef enum CandFate
{
	CAND_TARGET_NOT_FOUND,		/* no relation of that name in the query */
	CAND_NOT_EXAMINED,			/* target in query, never reached injection */
	CAND_UNKNOWN,				/* injected, no outcome observed */
	CAND_REJECTED,
	CAND_UNKNOWN_AFTER_ACCEPT,	/* accepted, later outcome not observed */
	CAND_SURVIVED_AT_POINT,		/* survived, displacement not covered */
	CAND_SURVIVED,				/* survived, no displacement observed */
	CAND_DISPLACED,
	CAND_UPPER,					/* accepted into an upper rel */
	CAND_ILLEGAL
}			CandFate;

static CandFate
propose_candidate_fate(PathEventLog *log, ProposedPathSpec * spec,
					   int *illegal_seq)
{
	bool		acc_covered = (log->coverage & PP_COV_ACCEPTED) != 0;
	bool		accepted = false,
				rejected = false,
				survived = false,
				displaced = false,
				upper = false;
	int			i;

	*illegal_seq = -1;
	if (!spec->injected)
		return spec->target_in_query ? CAND_NOT_EXAMINED :
			CAND_TARGET_NOT_FOUND;

	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];
		bool		ok = true;

		if (ev->source != PATH_SOURCE_PROPOSED ||
			!ev->label || strcmp(ev->label, spec->pathname) != 0)
			continue;

		if (ev->relids == NULL || bms_is_empty(ev->relids))
		{
			if (ev->kind == PEK_ACCEPTED)
			{
				ok = (accepted || survived) && !displaced;
				upper = true;
			}
		}
		else
		{
			switch (ev->kind)
			{
				case PEK_REJECTED:
					ok = !(accepted || survived || displaced);
					rejected = true;
					break;
				case PEK_ACCEPTED:
					ok = !(rejected || displaced);
					accepted = true;
					break;
				case PEK_SURVIVED:
					ok = !(rejected || displaced) && (accepted || !acc_covered);
					survived = true;
					break;
				case PEK_DISPLACED:
					ok = !rejected && (accepted || !acc_covered);
					displaced = true;
					break;
				default:
					ok = false;
					break;
			}
		}
		if (!ok)
		{
			*illegal_seq = ev->seq;
			return CAND_ILLEGAL;
		}
	}

	if (upper)
		return CAND_UPPER;
	if (rejected)
		return CAND_REJECTED;
	if (displaced)
		return CAND_DISPLACED;
	if (survived)
		return (log->coverage & PP_COV_DISPLACED) ? CAND_SURVIVED :
			CAND_SURVIVED_AT_POINT;
	if (accepted)
		return CAND_UNKNOWN_AFTER_ACCEPT;
	return CAND_UNKNOWN;
}

static const char *
cand_fate_text(CandFate f, bool json_form)
{
	switch (f)
	{
		case CAND_TARGET_NOT_FOUND:
			return json_form ? "target_not_found" :
				"target not found (no relation of that name in the query)";
		case CAND_NOT_EXAMINED:
			return json_form ? "not_examined" :
				"not examined (target in query, injection point not reached)";
		case CAND_REJECTED:
			return "rejected";
		case CAND_UNKNOWN_AFTER_ACCEPT:
			return json_form ? "unknown_after_accept" :
				"accepted -> later outcome not observed";
		case CAND_SURVIVED_AT_POINT:
			return json_form ? "survived_at_point" :
				"survived its add_path (later displacement not observed)";
		case CAND_SURVIVED:
			return "survived";
		case CAND_DISPLACED:
			return json_form ? "displaced" : "accepted -> displaced";
		case CAND_UPPER:
			return json_form ? "survived_upper" : "survived (reached upper rel)";
		case CAND_ILLEGAL:
			return json_form ? "inconsistent" : "INCONSISTENT (illegal transition)";
		case CAND_UNKNOWN:
		default:
			return json_form ? "unknown" : "unknown (outcome not observed)";
	}
}


/* -------------------------------------------------------------------------
 * render_propose_card - candidate-first view (replaces render_propose_text
 * as the default output for pathprobe_propose).
 *
 * Output:
 *   1. PATHPROBE PROPOSE header
 *   2. CANDIDATE FATE  <- main block
 *   3. CONTEXT
 *   4. SEARCH CONTEXT summary (secondary)
 * -------------------------------------------------------------------------
 */
static void
render_propose_card(PathProbeReport *report, StringInfo out,
					PathProbeOpts *opts, ProposedPathSpec * spec,
					bool injected, bool with_summary)
{
	PathEventLog *log = report->log;
	int			i;
	bool		wp_survived = false;
	bool		wp_rejected = false;
	Cost		wp_best_cost = 0.0;
	bool		wp_cost_set = false;
	NodeTag		winner_type = T_Invalid;
	Cost		winner_cost = 0.0;
	bool		winner_set = false;
	char		target_rel[32];
	bool		target_found = false;
	const char *fate;
	CandFate	cf;
	int			illegal_seq;
	Cost		gap;
	bool		gap_valid;
	const char *note = NULL;
	StringInfoData summary_buf;
	const char *sum_start;
	PathProbeOpts summary_opts;

	strlcpy(target_rel, "?", sizeof(target_rel));

	/* Retag CustomScan events -> PATH_SOURCE_PROPOSED before fate derivation */
	retag_proposed_events(log);

	/* Single pass: derive fate + effective winner */
	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];

		if (!target_found && ev->relids && !bms_is_empty(ev->relids) &&
			ev->kind == PEK_SURVIVED &&
			ev->source == PATH_SOURCE_REL_HOOK &&
			strcmp(ev->relname, spec->relname) == 0)
		{
			relids_to_str(ev->relids, target_rel, sizeof(target_rel));
			target_found = true;
		}

		if (ev->source == PATH_SOURCE_PROPOSED &&
			ev->label && strcmp(ev->label, spec->pathname) == 0)
		{
			if (!wp_cost_set || ev->cost_total < wp_best_cost)
			{
				wp_best_cost = ev->cost_total;
				wp_cost_set = true;
			}
			if (ev->kind == PEK_SURVIVED)
				wp_survived = true;
			if (ev->kind == PEK_REJECTED)
				wp_rejected = true;
		}
		else if (ev->kind == PEK_SURVIVED &&
				 ev->source == PATH_SOURCE_REL_HOOK &&
				 strcmp(ev->relname, spec->relname) == 0)
		{
			if (!winner_set || ev->cost_total < winner_cost)
			{
				winner_cost = ev->cost_total;
				winner_type = ev->pathtype;
				winner_set = true;
			}
		}
	}

	/* Derive fate string */
	cf = propose_candidate_fate(log, spec, &illegal_seq);
	fate = cand_fate_text(cf, false);
	if (cf == CAND_ILLEGAL)
		note = psprintf("event #%d broke the fate machine: "
						"either an unmodeled planner action or a probe bug",
						illegal_seq);

	/*
	 * gap = candidate_best_cost - winner_cost < 0: candidate would win on raw
	 * cost > 0: candidate genuinely lost
	 */
	gap_valid = (wp_cost_set && winner_set);
	gap = gap_valid ? (wp_best_cost - winner_cost) : 0.0;

	/* Policy-aware note */
	if (spec->winner_forbidden &&
		(cf == CAND_DISPLACED || cf == CAND_UNKNOWN_AFTER_ACCEPT))
	{
		if (gap_valid && gap < 0.0)
			note = "candidate would win on raw cost, "
				"but winner_forbidden policy prevented final win";
		else
			note = "winner_forbidden policy active; "
				"candidate competed but could not become cheapest";
	}
	else if (!spec->winner_forbidden && wp_survived)
		note = "candidate won -- execution will fail in v0 "
			"(diagnostic mode only)";
	else if (wp_rejected && gap_valid && gap > 0.0)
		note = "candidate genuinely lost on cost";
	else if (wp_rejected && spec->winner_forbidden)
		note = "disabled_nodes=1 (winner_forbidden) caused immediate rejection";

	/* ------------------------------------------------------------------
     * 1. Header
     * ------------------------------------------------------------------ */
	appendStringInfoString(out,
						   "PATHPROBE PROPOSE\n"
						   "----------------------------------------------------------------------\n");
	appendStringInfo(out, "target : %s (%s)\n", target_rel, spec->relname);
	appendStringInfo(out, "mode   : %s\n",
					 spec->winner_forbidden ? "winner_forbidden=on" : "winner_forbidden=off");

	if (!injected)
	{
		appendStringInfo(out, "fate   : %s\n", fate);
		return;
	}
	appendStringInfoChar(out, '\n');

	/* ------------------------------------------------------------------
     * 2. Candidate fate  (main block - always first)
     * ------------------------------------------------------------------ */
	appendStringInfoString(out,
						   "CANDIDATE FATE\n"
						   "----------------------------------------------------------------------\n");
	appendStringInfo(out, "pathname : %s\n", spec->pathname);
	appendStringInfo(out, "provider : %s\n", spec->provider);
	appendStringInfoString(out, "type     : CustomScan\n");
	appendStringInfo(out, "cost     : (%.2f..%.2f)\n",
					 spec->startup_cost, spec->total_cost);
	appendStringInfo(out, "rows     : %.0f\n", spec->rows);
	appendStringInfoChar(out, '\n');
	appendStringInfo(out, "fate     : %s%s\n", fate,
					 log->events_truncated ?
					 "  [events truncated: fate unreliable]" : "");
	if (winner_set)
		appendStringInfo(out, "winner   : %s @ %.2f\n",
						 pathtype_name(winner_type), winner_cost);
	if (gap_valid)
		appendStringInfo(out, "gap      : %+.2f\n", gap);
	if (note)
		appendStringInfo(out, "note     : %s\n", note);
	appendStringInfoChar(out, '\n');

	/* ------------------------------------------------------------------
     * 3. Context
     * ------------------------------------------------------------------ */
	if (wp_cost_set || winner_set)
	{
		appendStringInfoString(out,
							   "CONTEXT\n"
							   "----------------------------------------------------------------------\n");
		if (wp_cost_set)
			appendStringInfo(out, "raw best         : %s @ %.2f\n",
							 spec->pathname, wp_best_cost);
		if (winner_set)
			appendStringInfo(out, "effective winner : %s @ %.2f\n",
							 pathtype_name(winner_type), winner_cost);
		appendStringInfoChar(out, '\n');
	}

	/* ------------------------------------------------------------------
     * 4. Summary (secondary view)
     * ------------------------------------------------------------------ */
	initStringInfo(&summary_buf);
	summary_opts.verbose = false;
	summary_opts.log_accepted = false;
	render_text(report, &summary_buf, &summary_opts);
	sum_start = strstr(summary_buf.data, "Summary (per relation");
	if (with_summary && sum_start)
	{
		appendStringInfoString(out,
							   "SEARCH CONTEXT (summary)\n"
							   "----------------------------------------------------------------------\n");
		appendStringInfoString(out, sum_start);
	}
}

/* -------------------------------------------------------------------------
 * SQL wrapper: pathprobe_propose_json(query text, spec text) RETURNS text
 *
 * Same as pathprobe_propose() but JSON output.
 * Extends standard pathprobe_json() structure with:
 *   "candidate": {pathname, provider, cost_total, rows, winner_forbidden}
 *   "target":    {relname}
 *   "fate":      {fate, gap, winner_strategy, winner_cost, note}
 * Events tagged src="wp" for proposed path.
 * -------------------------------------------------------------------------
 */

/* per-candidate fate derivation for the JSON candidates array */
static void
propose_candidate_json(StringInfo out, PathEventLog *log,
					   ProposedPathSpec * spec)
{
	int			i;
	bool		wp_cost_set = false,
				winner_set = false;
	Cost		wp_best_cost = 0.0,
				winner_cost = 0.0;
	NodeTag		winner_type = T_Invalid;
	int			n_acc = 0,
				n_rej = 0,
				n_surv = 0,
				n_disp = 0;
	const char *fate;

	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];

		if (ev->source == PATH_SOURCE_PROPOSED &&
			ev->label && strcmp(ev->label, spec->pathname) == 0)
		{
			if (!wp_cost_set || ev->cost_total < wp_best_cost)
			{
				wp_best_cost = ev->cost_total;
				wp_cost_set = true;
			}
			if (ev->kind == PEK_ACCEPTED)
				n_acc++;
			else if (ev->kind == PEK_REJECTED)
				n_rej++;
			else if (ev->kind == PEK_SURVIVED)
				n_surv++;
			else if (ev->kind == PEK_DISPLACED)
				n_disp++;
		}
		else if (ev->kind == PEK_SURVIVED &&
				 ev->source == PATH_SOURCE_REL_HOOK &&
				 strcmp(ev->relname, spec->relname) == 0)
		{
			if (!winner_set || ev->cost_total < winner_cost)
			{
				winner_cost = ev->cost_total;
				winner_type = ev->pathtype;
				winner_set = true;
			}
		}
	}

	{
		int			illegal_seq;

		fate = cand_fate_text(propose_candidate_fate(log, spec, &illegal_seq),
							  true);
	}

	appendStringInfoString(out, "{\"pathname\":");
	pathprobe_json_str(out, spec->pathname);
	appendStringInfoString(out, ",\"target\":");
	pathprobe_json_str(out, spec->relname);
	appendStringInfoString(out, ",\"provider\":");
	pathprobe_json_str(out, spec->provider);
	appendStringInfo(out, ",\"injected\":%s",
					 spec->injected ? "true" : "false");
	appendStringInfo(out, ",\"winner_forbidden\":%s",
					 spec->winner_forbidden ? "true" : "false");
	appendStringInfo(out, ",\"cost_total\":%.2f,\"rows\":%.0f",
					 spec->total_cost, spec->rows);
	appendStringInfoString(out, ",\"fate\":");
	pathprobe_json_str(out, fate);
	if (winner_set)
	{
		appendStringInfoString(out, ",\"winner_strategy\":");
		pathprobe_json_str(out, pathtype_name(winner_type));
		appendStringInfo(out, ",\"winner_cost\":%.2f", winner_cost);
	}
	if (wp_cost_set && winner_set)
		appendStringInfo(out, ",\"gap\":%.2f", wp_best_cost - winner_cost);
	appendStringInfo(out,
					 ",\"event_counts\":{\"accepted\":%d,\"rejected\":%d,"
					 "\"survived\":%d,\"displaced\":%d}",
					 n_acc, n_rej, n_surv, n_disp);
	appendStringInfoChar(out, '}');
}

PG_FUNCTION_INFO_V1(pathprobe_propose_json);

Datum
pathprobe_propose_json(PG_FUNCTION_ARGS)
{
	text	   *query_text = PG_GETARG_TEXT_PP(0);
	text	   *spec_text = PG_GETARG_TEXT_PP(1);
	char	   *query_str = text_to_cstring(query_text);

	char	   *spec_str = text_to_cstring(spec_text);
	Datum		spec_datum;
	Jsonb	   *spec_jb;
	List	   *specs;
	List	   *hypo;
	bool		advise;
	ProposedPathSpec *spec;
	bool		explicit_candidate = true;
	PathProbeOpts opts = {false, true};
	PathProbeAccess access;
	PathProbeReport *report;
	StringInfoData out;
	StringInfoData json_body;

	/* Parse spec */
	if (strlen(query_str) > (size_t) pathprobe_max_query_length)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("pathprobe: query longer than pathprobe.max_query_length (%d)",
							   pathprobe_max_query_length)));

	pathprobe_propose_register_label_provider();

	spec_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(spec_str));
	spec_jb = DatumGetJsonbP(spec_datum);
	advise = spec_get_advise(spec_jb);
	hypo = list_concat(parse_hypo_array(spec_jb), parse_hypo_sql(spec_jb));
	specs = ((hypo != NIL || advise) && !spec_has_candidates(spec_jb)) ? NIL
		: parse_spec_multi(spec_jb);
	spec = specs ? (ProposedPathSpec *) linitial(specs) : NULL;

	/* Force log_accepted=true so proposed path accepted events are visible */
	{
		bool		saved_log_accepted = pathprobe_log_accepted;

		pathprobe_log_accepted = true;
		/* hints + baseline are applied inside, only after authorization */
		report = pathprobe_propose_internal(query_str, specs, &hypo, advise,
											spec_get_bool(spec_jb, "advise_skip_prefix"),
											&opts, &access, spec_jb);
		pathprobe_log_accepted = saved_log_accepted;
	}

	/* Retag wp events */
	retag_proposed_events(report->log);

	if (spec == NULL && specs == NIL)
	{
		/* hypo-only or advise-only call: there is no candidate to report on */
		explicit_candidate = false;
		spec = (ProposedPathSpec *) palloc0(sizeof(ProposedPathSpec));
		spec->pathname = "";
		spec->relname = "";
		spec->provider = "";
	}

	/* Derive fate (same logic as render_propose_card) */
	{
		PathEventLog *log = report->log;
		int			i;
		bool		wp_rejected = false;
		Cost		wp_best_cost = 0.0;
		bool		wp_cost_set = false;
		NodeTag		winner_type = T_Invalid;
		Cost		winner_cost = 0.0;
		bool		winner_set = false;
		const char *fate;
		CandFate	cf;
		int			illegal_seq;
		Cost		gap;
		bool		gap_valid;
		const char *note = NULL;

		for (i = 0; i < log->nevents; i++)
		{
			PathEvent  *ev = &log->events[i];

			if (ev->source == PATH_SOURCE_PROPOSED &&
				ev->label && strcmp(ev->label, spec->pathname) == 0)
			{
				if (!wp_cost_set || ev->cost_total < wp_best_cost)
				{
					wp_best_cost = ev->cost_total;
					wp_cost_set = true;
				}
				if (ev->kind == PEK_REJECTED)
					wp_rejected = true;
			}
			else if (ev->kind == PEK_SURVIVED &&
					 ev->source == PATH_SOURCE_REL_HOOK &&
					 strcmp(ev->relname, spec->relname) == 0)
			{
				if (!winner_set || ev->cost_total < winner_cost)
				{
					winner_cost = ev->cost_total;
					winner_type = ev->pathtype;
					winner_set = true;
				}
			}
		}

		/* the same derivation as candidates[] and the text card */
		cf = propose_candidate_fate(log, spec, &illegal_seq);
		fate = cand_fate_text(cf, true);

		gap_valid = (wp_cost_set && winner_set);
		gap = gap_valid ? (wp_best_cost - winner_cost) : 0.0;

		if (spec->winner_forbidden &&
			(cf == CAND_DISPLACED || cf == CAND_UNKNOWN_AFTER_ACCEPT))
			note = gap_valid && gap < 0.0
				? "candidate_would_win_policy_prevented"
				: "winner_forbidden_active";
		else if (wp_rejected && spec->winner_forbidden)
			note = "disabled_nodes_caused_rejection";
		else if (wp_rejected && gap_valid && gap > 0.0)
			note = "genuine_cost_loss";

		/* Build JSON */
		initStringInfo(&json_body);
		render_json(report, &json_body, &opts, query_str);

		initStringInfo(&out);

		/* Start our extended object */
		appendStringInfoString(&out, "{\n");

		/* candidate block */
		appendStringInfoString(&out, "  \"candidate\": {\"pathname\":");
		pathprobe_json_str(&out, spec->pathname);
		appendStringInfoString(&out, ",\"provider\":");
		pathprobe_json_str(&out, spec->provider);
		appendStringInfo(&out,
						 ",\"cost_total\":%.2f,\"rows\":%.0f,\"winner_forbidden\":%s},\n",
						 spec->total_cost, spec->rows,
						 spec->winner_forbidden ? "true" : "false");

		/* target block */
		appendStringInfoString(&out, "  \"target\": {\"relname\":");
		pathprobe_json_str(&out, spec->relname);
		appendStringInfoString(&out, "},\n");

		/*
		 * Legacy single-candidate fate.  Without an explicit candidate there
		 * is nothing to report, so the block is omitted rather than claiming
		 * a fate for the placeholder spec.
		 */
		if (explicit_candidate)
		{
			appendStringInfo(&out,
							 "  \"fate\": {\"fate\":\"%s\"", fate);
			if (winner_set)
				appendStringInfo(&out,
								 ",\"winner_strategy\":\"%s\",\"winner_cost\":%.2f",
								 pathtype_name(winner_type), winner_cost);
			if (gap_valid)
				appendStringInfo(&out, ",\"gap\":%.2f", gap);
			if (note)
			{
				appendStringInfoString(&out, ",\"note\":");
				pathprobe_json_str(&out, note);
			}
			appendStringInfoString(&out, "},\n");
		}

		/* advisor summary block (machine model, no text parsing needed) */
		appendStringInfoString(&out, "  \"advisor\": {");
		appendStringInfo(&out, "\"enabled\":%s,\"truncated\":%s",
						 advise ? "true" : "false",
						 report->advisor_truncated ? "true" : "false");
		if (report->advise_atoms && report->advise_atoms[0])
		{
			appendStringInfoString(&out, ",\"atoms\":");
			pathprobe_json_str(&out, report->advise_atoms);
		}
		if (report->baseline_cost >= 0)
			appendStringInfo(&out, ",\"baseline_cost\":%.2f",
							 report->baseline_cost);
		if (report->plan_cost >= 0)
			appendStringInfo(&out, ",\"plan_cost\":%.2f", report->plan_cost);
		if (report->baseline_cost >= 0 && report->plan_cost >= 0)
			appendStringInfo(&out, ",\"query_level_benefit\":%.2f",
							 report->baseline_cost - report->plan_cost);
		appendStringInfoString(&out, "},\n");

		/* hypothetical indexes: explicit machine fields incl. in_plan */
		appendStringInfoString(&out, "  \"hypo_indexes\": [");
		{
			ListCell   *lch;
			bool		hfirst = true;

			foreach(lch, hypo)
			{
				HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lch);
				HypoCounts	hc;
				HypoFate	hf;
				const char *reason;

				hypo_count_events(report->log, h, &hc, false);
				hf = hypo_fate(report->log, h, &reason);
				if (!hfirst)
					appendStringInfoChar(&out, ',');
				hfirst = false;
				appendStringInfoString(&out, "{\"name\":");
				pathprobe_json_str(&out, h->name);
				appendStringInfoString(&out, ",\"relname\":");
				pathprobe_json_str(&out, h->relname);
				appendStringInfoString(&out, ",\"ddl\":");
				pathprobe_json_str(&out, h->sql);
				appendStringInfo(&out, ",\"injected\":%s,\"in_plan\":%s",
								 h->injected ? "true" : "false",
								 h->in_plan ? "true" : "false");
				appendStringInfo(&out,
								 ",\"accepted\":%d,\"rejected\":%d,\"survived\":%d"
								 ",\"displaced\":%d",
								 hc.accepted, hc.rejected, hc.survived,
								 hc.displaced);
				if (hc.best_set)
					appendStringInfo(&out, ",\"best_cost\":%.2f", hc.best);
				appendStringInfo(&out, ",\"fate\":\"%s\"", hypo_fate_text(hf));
				if (reason)
					appendStringInfo(&out, ",\"fate_reason\":\"%s\"", reason);
				if (report->log->coverage & PP_COV_INDEX_BUILD)
				{
					appendStringInfo(&out,
									 ",\"index_build\":{\"calls\":{\"plain\":%d"
									 ",\"saop_bitmap\":%d,\"or_arm\":%d}"
									 ",\"generated\":%d,\"not_generated\":[",
									 h->build_plain, h->build_saop,
									 h->build_or_arm, h->build_generated);
					hypo_notgen_append(&out, h->build_notgen, true);
					appendStringInfoString(&out, "]}");
				}
				else
					appendStringInfoString(&out, ",\"index_build\":null");
				appendStringInfoChar(&out, '}');
			}
		}
		appendStringInfoString(&out, "],\n");

		/* per-candidate fates (multi-injection) */
		appendStringInfoString(&out, "  \"candidates\": [");
		{
			ListCell   *lc2;

			foreach(lc2, specs)
			{
				if (lc2 != list_head(specs))
					appendStringInfoString(&out, ",");
				propose_candidate_json(&out, report->log,
									   (ProposedPathSpec *) lfirst(lc2));
			}
		}
		appendStringInfoString(&out, "],\n");

		/* Append rest of standard render_json output (skip opening "{\n") */
		{
			const char *body = json_body.data;

			if (body[0] == '{')
			{
				body++;
				if (*body == '\n')
					body++;
			}
			appendStringInfoString(&out, body);
		}
	}

	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
