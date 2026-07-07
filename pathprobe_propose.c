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
#include "utils/ruleutils.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
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

static void retag_proposed_events(PathEventLog *log);


extern bool pathprobe_log_accepted; /* GUC from pathprobe.c */

/* ----------
 * ProposedPathSpec - parsed from JSON spec
 * ----------
 */
#define PATHPROBE_MAX_HYPO_COLS 8

typedef struct ProposedPathSpec
{
	/* target */
	char	   *relname;		/* target base relation name */

	/* candidate identity */
	char	   *pathname;		/* stable human name for this candidate */
	char	   *provider;		/* logical source: "duckdb", "mock", etc. */

	/* cost */
	Cost		startup_cost;
	Cost		total_cost;
	double		rows;
	int			width;

	/* planner-visible properties */
	bool		parameterized;
	bool		parallel_safe;
	bool		parallel_aware;
	int			npathkeys;		/* planner.pathkeys column names */
	char	   *pathkey_cols[PATHPROBE_MAX_HYPO_COLS];
	int			disabled_nodes; /* base; winner_forbidden adds 1 more */

	/* mode */
	bool		winner_forbidden;	/* add disabled_nodes=1, prevent winning */

	/* runtime */
	bool		injected;		/* set once this candidate was injected */
}			ProposedPathSpec;

/* ----------
 * HypoIndexSpec - hypothetical btree index (v0: plain columns only)
 *
 * Injected HypoPG-style, but this tree has no get_relation_info_hook, so we
 * inject at set_rel_pathlist time: temporarily set rel->indexlist to the
 * hypothetical entry alone and call create_index_paths() again.  The
 * resulting IndexPaths enter ordinary add_path() competition against the
 * already-generated frontier.  Plans are never executed here (propose only
 * plans and discards), so a hypothetical index can never reach the executor.
 * ----------
 */

typedef struct HypoIndexSpec
{
	char	   *name;			/* label: events tagged "hypo:<name>" */
	char	   *relname;
	int			ncols;
	int			nkeys;			/* key columns; 0 = all ncols are keys */
	char	   *colnames[PATHPROBE_MAX_HYPO_COLS];

	AttrNumber *adv_attnos;		/* advisor-generated signature (or NULL) */
	Oid			amoid;			/* index AM (stmt form); InvalidOid = btree */

	/* full-grammar variant: transformed CREATE INDEX statement (or NULL) */
	IndexStmt  *stmt;
	char	   *sql;			/* original statement, for display */
	Oid			relid;			/* resolved target; authoritative for stmt form */

	/* runtime */
	bool		injected;
	bool		in_plan;		/* fake oid found in the final plan */
	IndexOptInfo *ioi;			/* identity for event labeling */
}			HypoIndexSpec;

static HypoIndexSpec *hypo_spec_from_sql(char *sql, int idx);

/* ----------
 * ProposedPathState - active during one planning call
 * Valid only between set_rel_pathlist_hook activation and return.
 * ----------
 */
typedef struct ProposedPathState
{
	List	   *specs;			/* ProposedPathSpec* list, one per candidate */
	List	   *hypo;			/* HypoIndexSpec* list */
	bool		advise;			/* auto-generate index candidates per rel */
	bool		advise_skip_prefix; /* skip candidates that are a prefix of an existing index */
	char	   *atoms_str;		/* printable atom stage (advise mode) */
	int			advise_generated;	/* advisor candidates so far (cap) */
	bool		advise_truncated;
	PathProbeAccess *access;	/* ACL decision for this probe (never NULL) */
	PlannerInfo *root;			/* for pathkeys lookup */
	bool		injected;		/* did we inject at least one? */
	bool		winner;			/* did it become cheapest_total? */
}			ProposedPathState;

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
static bool
json_get_bool(Jsonb *jb, const char *path1, const char *path2, bool defval)
{
	JsonbValue *jv;
	JsonbValue	key1,
				key2;
	JsonbContainer *c;

	if (!JB_ROOT_IS_OBJECT(jb))
		return defval;

	key1.type = jbvString;
	key1.val.string.val = (char *) path1;
	key1.val.string.len = strlen(path1);
	jv = findJsonbValueFromContainer(&jb->root, JB_FOBJECT, &key1);
	if (!jv || jv->type != jbvBinary)
		return defval;

	c = (JsonbContainer *) jv->val.binary.data;
	key2.type = jbvString;
	key2.val.string.val = (char *) path2;
	key2.val.string.len = strlen(path2);
	jv = findJsonbValueFromContainer(c, JB_FOBJECT, &key2);
	if (!jv || jv->type != jbvBool)
		return defval;

	return jv->val.boolean;
}

static double
json_get_number(Jsonb *jb, const char *path1, const char *path2, double defval)
{
	JsonbValue *jv;
	JsonbValue	key1,
				key2;
	JsonbContainer *c;

	if (!JB_ROOT_IS_OBJECT(jb))
		return defval;

	key1.type = jbvString;
	key1.val.string.val = (char *) path1;
	key1.val.string.len = strlen(path1);
	jv = findJsonbValueFromContainer(&jb->root, JB_FOBJECT, &key1);
	if (!jv || jv->type != jbvBinary)
		return defval;

	c = (JsonbContainer *) jv->val.binary.data;
	key2.type = jbvString;
	key2.val.string.val = (char *) path2;
	key2.val.string.len = strlen(path2);
	jv = findJsonbValueFromContainer(c, JB_FOBJECT, &key2);
	if (!jv)
		return defval;
	if (jv->type == jbvNumeric)
		return DatumGetFloat8(DirectFunctionCall1(numeric_float8,
												  NumericGetDatum(jv->val.numeric)));
	return defval;
}

static char *
json_get_string(Jsonb *jb, const char *path1, const char *path2,
				const char *defval)
{
	JsonbValue *jv;
	JsonbValue	key1,
				key2;
	JsonbContainer *c;
	char	   *result;

	if (!JB_ROOT_IS_OBJECT(jb))
		return defval ? pstrdup(defval) : NULL;

	key1.type = jbvString;
	key1.val.string.val = (char *) path1;
	key1.val.string.len = strlen(path1);
	jv = findJsonbValueFromContainer(&jb->root, JB_FOBJECT, &key1);
	if (!jv || jv->type != jbvBinary)
		return defval ? pstrdup(defval) : NULL;

	c = (JsonbContainer *) jv->val.binary.data;
	key2.type = jbvString;
	key2.val.string.val = (char *) path2;
	key2.val.string.len = strlen(path2);
	jv = findJsonbValueFromContainer(c, JB_FOBJECT, &key2);
	if (!jv || jv->type != jbvString)
		return defval ? pstrdup(defval) : NULL;

	result = palloc(jv->val.string.len + 1);
	memcpy(result, jv->val.string.val, jv->val.string.len);
	result[jv->val.string.len] = '\0';
	return result;
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
		JsonbValue	k,
				   *pv;

		k.type = jbvString;
		k.val.string.val = "planner";
		k.val.string.len = strlen("planner");
		pv = findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &k);
		if (pv && pv->type == jbvBinary)
		{
			JsonbValue	k2,
					   *av;

			k2.type = jbvString;
			k2.val.string.val = "pathkeys";
			k2.val.string.len = strlen("pathkeys");
			av = findJsonbValueFromContainer(pv->val.binary.data,
											 JB_FOBJECT, &k2);
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
	JsonbValue	k,
			   *arr;
	JsonbContainer *ac;
	int			n,
				i;

	k.type = jbvString;
	k.val.string.val = unconstify(char *, key);
	k.val.string.len = strlen(key);
	arr = findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &k);
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
	JsonbValue	k,
			   *hv;
	Jsonb	   *hints;
	int			nest;

	if (!JB_ROOT_IS_OBJECT(spec_jb))
		return 0;
	k.type = jbvString;
	k.val.string.val = "hints";
	k.val.string.len = strlen("hints");
	hv = findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &k);
	if (!hv)
		return 0;
	if (hv->type != jbvBinary || !JsonContainerIsObject(hv->val.binary.data))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("pathprobe: \"hints\" must be an object")));
	hints = JsonbValueToJsonb(hv);

	nest = NewGUCNestLevel();

	k.val.string.val = "allow_only";
	k.val.string.len = strlen("allow_only");
	if (findJsonbValueFromContainer(&hints->root, JB_FOBJECT, &k))
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
	JsonbValue	k,
			   *v;

	if (!JB_ROOT_IS_OBJECT(spec_jb))
		return false;
	k.type = jbvString;
	k.val.string.val = unconstify(char *, key);
	k.val.string.len = strlen(key);
	v = findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &k);
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
	JsonbValue	key;

	if (!JB_ROOT_IS_OBJECT(spec_jb))
		return true;
	key.type = jbvString;
	key.val.string.val = "candidates";
	key.val.string.len = strlen("candidates");
	if (findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &key))
		return true;
	key.val.string.val = "target";
	key.val.string.len = strlen("target");
	return findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &key) != NULL;
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
		List	   *querytree_list;
		List	   *plantree_list;

		{
			Oid		   *ptypes = NULL;
			int			nparams = 0;

			querytree_list = pg_analyze_and_rewrite_varparams(parsetree,
															   query_string,
															   &ptypes,
															   &nparams,
															   NULL);
		}
		plantree_list = pg_plan_queries(querytree_list, query_string,
										CURSOR_OPT_PARALLEL_OK, NULL);
		if (plantree_list != NIL)
		{
			PlannedStmt *ps = linitial_node(PlannedStmt, plantree_list);

			if (ps->planTree)
				cost = ps->planTree->total_cost;
		}
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

/*
 * Advisor (increment 1, btree): derive index candidates for this rel from
 * rel->baserestrictinfo and root->query_pathkeys, then feed them into the
 * ordinary hypothetical-index competition.  Capability test is catalog
 * driven: an operator qualifies iff it has a strategy in the column's
 * default btree opclass family (the same opclass a real CREATE INDEX would
 * use).  Candidates: single-column per qualifying qual (equalities first),
 * one composite (equalities + first range column), one ASC ordered set from
 * query_pathkeys.  Hard cap below keeps the run to one planning pass.
 */
#define PATHPROBE_ADVISE_MAX 6

/*
 * Index atom: a unique indexable argument collected from the query.
 * attno > 0 for plain columns; attno == 0 for expression atoms (expr set).
 * Extraction sources:
 *   - EquivalenceClasses with ec_has_const: the planner already deduped
 *     equality arguments, stripped RelabelType and applied transitivity
 *     (a = b AND b = 5 yields the atom a as well);
 *   - baserestrictinfo: side selection by the precomputed
 *     rinfo->left_relids/right_relids (the rule match_opclause_to_indexcol
 *     itself uses), plus degenerate SAOP/NullTest branches which have no
 *     left/right relids.
 */
typedef struct AdvCol
{
	AttrNumber	attno;			/* 0 = expression atom */
	Expr	   *expr;			/* expression when attno == 0 */
	bool		is_eq;
}			AdvCol;

/* catalog capability test: type has a default btree opclass; op (if any)
 * has a strategy in its family */
static int
adv_btree_strategy(Oid opno, Oid atttype)
{
	Oid			opclass = GetDefaultOpClass(atttype, BTREE_AM_OID);

	if (!OidIsValid(opclass))
		return 0;
	if (!OidIsValid(opno))
		return BTEqualStrategyNumber;	/* IS NULL / EC member */
	return get_op_opfamily_strategy(opno, get_opclass_family(opclass));
}

static bool
adv_atom_from_node(RelOptInfo *rel, Node *node, Oid opno, AdvCol * out)
{
	if (node == NULL)
		return false;
	if (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	if (contain_volatile_functions(node))
		return false;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) node;

		if (v->varno != rel->relid || v->varattno <= 0)
			return false;
		out->attno = v->varattno;
		out->expr = NULL;
	}
	else
	{
		if (IsA(node, Const) || IsA(node, Param))
			return false;
		out->attno = 0;
		out->expr = (Expr *) node;
	}
	{
		int			strat = adv_btree_strategy(opno, exprType(node));

		if (strat <= 0)
			return false;
		out->is_eq = (strat == BTEqualStrategyNumber);
	}
	return true;
}

static bool
adv_col_btree_qual(RelOptInfo *rel, RangeTblEntry *rte, RestrictInfo *rinfo,
				   AdvCol * out)
{
	Expr	   *clause = rinfo->clause;

	/* IS NULL: indexable via btree amsearchnulls; equality-like */
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		if (nt->nulltesttype != IS_NULL || nt->argisrow)
			return false;		/* IS NOT NULL skipped: rarely selective */
		return adv_atom_from_node(rel, (Node *) nt->arg, InvalidOid, out);
	}

	/* col = ANY(array): btree native via amsearcharray.  SAOP has no
	 * precomputed left/right relids, hence the explicit array-side check. */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

		if (!saop->useOr || list_length(saop->args) != 2)
			return false;
		if (contain_var_clause((Node *) lsecond(saop->args)))
			return false;
		return adv_atom_from_node(rel, (Node *) linitial(saop->args),
								  saop->opno, out);
	}

	/* binary operator: pick the side by precomputed relids, the same rule
	 * match_opclause_to_indexcol applies (indxpath.c) */
	if (IsA(clause, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) clause;

		if (list_length(op->args) != 2)
			return false;
		if (bms_is_subset(rinfo->left_relids, rel->relids) &&
			bms_is_empty(rinfo->right_relids))
			return adv_atom_from_node(rel, (Node *) linitial(op->args),
									  op->opno, out);
		if (bms_is_subset(rinfo->right_relids, rel->relids) &&
			bms_is_empty(rinfo->left_relids))
		{
			Oid			commut = get_commutator(op->opno);

			/*
			 * A non-commutable operator (LIKE etc.) cannot become an
			 * index qual with the Var on the right; InvalidOid must not
			 * leak into the strategy test, which treats it as the
			 * equality-like marker for IS NULL / EC / boolean callers.
			 */
			if (!OidIsValid(commut))
				return false;
			return adv_atom_from_node(rel, (Node *) lsecond(op->args),
									  commut, out);
		}
		return false;
	}

	/*
	 * Boolean quals, mirroring match_boolean_index_clause (indxpath.c):
	 * bare boolean clause (col / f(x)), NOT arg, BooleanTest IS TRUE /
	 * IS FALSE.  All are equality-like (col = true/false) for a btree
	 * candidate.  Other BooleanTest forms are not indexed by the core
	 * either.
	 */
	if (IsA(clause, BoolExpr) &&
		((BoolExpr *) clause)->boolop == NOT_EXPR &&
		list_length(((BoolExpr *) clause)->args) == 1)
	{
		AdvCol		c;

		if (adv_atom_from_node(rel,
							   (Node *) linitial(((BoolExpr *) clause)->args),
							   InvalidOid, &c) &&
			exprType((Node *) linitial(((BoolExpr *) clause)->args)) == BOOLOID)
		{
			*out = c;
			out->is_eq = true;
			return true;
		}
		return false;
	}
	if (IsA(clause, BooleanTest))
	{
		BooleanTest *bt = (BooleanTest *) clause;

		if (bt->booltesttype != IS_TRUE && bt->booltesttype != IS_FALSE)
			return false;
		if (adv_atom_from_node(rel, (Node *) bt->arg, InvalidOid, out))
		{
			out->is_eq = true;
			return true;
		}
		return false;
	}
	if (exprType((Node *) clause) == BOOLOID &&
		adv_atom_from_node(rel, (Node *) clause, InvalidOid, out))
	{
		out->is_eq = true;
		return true;
	}
	return false;
}

/*
 * adv_atoms_from_ecs - equality atoms from the planner's own dedup:
 * every non-const member of a const-bearing EquivalenceClass that belongs
 * entirely to this rel.  Transitive equalities come for free.
 */
static int
adv_atoms_from_ecs(PlannerInfo *root, RelOptInfo *rel,
				   AdvCol * cols, int ncols, int maxcols)
{
	ListCell   *lc;

	foreach(lc, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
		ListCell   *lm;

		if (!ec->ec_has_const || !bms_overlap(ec->ec_relids, rel->relids))
			continue;
		foreach(lm, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lm);
			AdvCol		c;
			int			i;

			if (em->em_is_const ||
				!bms_is_subset(em->em_relids, rel->relids) ||
				bms_is_empty(em->em_relids))
				continue;
			if (!adv_atom_from_node(rel, (Node *) em->em_expr,
									InvalidOid, &c))
				continue;
			c.is_eq = true;
			for (i = 0; i < ncols; i++)
				if ((c.attno > 0 && cols[i].attno == c.attno) ||
					(c.attno == 0 && cols[i].attno == 0 &&
					 equal(cols[i].expr, c.expr)))
				{
					cols[i].is_eq = true;
					break;
				}
			if (i == ncols && ncols < maxcols)
				cols[ncols++] = c;
		}
	}
	return ncols;
}




/*
 * Candidate lattice: a candidate or a real plain-btree index is a node
 * (K = key columns, I = trailing/INCLUDE columns).  R covers C when C.K is
 * a prefix of R.K and C.I fits into R's remaining columns.  Coverage class
 * drives dedup uniformly:
 *   ADV_COV_EQUAL - same key list (duplicate by capability): always skip;
 *   ADV_COV_WIDER - strictly wider key list (heuristic: the wide index
 *                   serves the same quals at a page-width penalty): skip
 *                   only under advise_skip_prefix.
 * The reverse direction (candidate keys wider than any existing index) is
 * never reduced - that is a qualitatively different access.
 */
typedef enum AdvCoverage
{
	ADV_COV_NONE,
	ADV_COV_EQUAL,
	ADV_COV_WIDER
}			AdvCoverage;

static AdvCoverage
adv_coverage(RelOptInfo *rel, AttrNumber *attnos, int n, int nkeys)
{
	AdvCoverage best = ADV_COV_NONE;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ioi = (IndexOptInfo *) lfirst(lc);
		int			i,
					j;

		if (ioi->relam != BTREE_AM_OID || ioi->hypothetical ||
			ioi->indexprs != NIL || ioi->indpred != NIL)
			continue;
		if (ioi->nkeycolumns < nkeys)
			continue;			/* candidate keys wider: never reduced */
		for (i = 0; i < nkeys; i++)
			if (ioi->indexkeys[i] != attnos[i])
				break;
		if (i < nkeys)
			continue;
		/* include part must fit into R's remaining columns */
		for (i = nkeys; i < n; i++)
		{
			for (j = nkeys; j < ioi->ncolumns; j++)
				if (ioi->indexkeys[j] == attnos[i])
					break;
			if (j == ioi->ncolumns)
				break;
		}
		if (i < n)
			continue;
		if (ioi->nkeycolumns == nkeys)
			return ADV_COV_EQUAL;
		best = ADV_COV_WIDER;
	}
	return best;
}

static bool
adv_cap_reached(ProposedPathState * state)
{
	int			cap = pathprobe_agent_mode ?
		Min(pathprobe_max_advisor_candidates, 16) :
		pathprobe_max_advisor_candidates;

	if (state->advise_generated >= cap)
	{
		state->advise_truncated = true;
		return true;
	}
	state->advise_generated++;
	return false;
}

/*
 * adv_skip_candidate - uniform dedup gate for all generation sites:
 * in-run exact signature, then lattice coverage by existing indexes.
 */
static bool
adv_skip_candidate(ProposedPathState * state, RelOptInfo *rel,
				   AttrNumber *attnos, int n, int nkeys)
{
	ListCell   *lc;
	AdvCoverage cov;

	foreach(lc, state->hypo)
	{
		HypoIndexSpec *h = (HypoIndexSpec *) lfirst(lc);
		int			i;

		if (h->adv_attnos == NULL || h->ncols != n ||
			(h->nkeys > 0 ? h->nkeys : h->ncols) != nkeys)
			continue;
		for (i = 0; i < n; i++)
			if (h->adv_attnos[i] != attnos[i])
				break;
		if (i == n)
			return true;
	}
	cov = adv_coverage(rel, attnos, n, nkeys);
	if (cov == ADV_COV_EQUAL)
		return true;
	if (cov == ADV_COV_WIDER && state->advise_skip_prefix)
		return true;
	return false;
}

static HypoIndexSpec *
adv_make_spec(RangeTblEntry *rte, const char *relname,
			  AttrNumber *attnos, int n, int nkeys, int seq)
{
	HypoIndexSpec *h = (HypoIndexSpec *) palloc0(sizeof(HypoIndexSpec));
	StringInfoData ddl;
	int			i;

	if (nkeys <= 0 || nkeys > n)
		nkeys = n;
	h->relname = pstrdup(relname);
	h->name = psprintf("adv_%d", seq);
	h->ncols = n;
	h->nkeys = nkeys;
	h->adv_attnos = (AttrNumber *) palloc(sizeof(AttrNumber) * n);
	memcpy(h->adv_attnos, attnos, sizeof(AttrNumber) * n);
	initStringInfo(&ddl);
	appendStringInfo(&ddl, "CREATE INDEX %s ON %s (", h->name, relname);
	for (i = 0; i < nkeys; i++)
	{
		h->colnames[i] = get_attname(rte->relid, attnos[i], false);
		appendStringInfo(&ddl, "%s%s", i ? ", " : "", h->colnames[i]);
	}
	appendStringInfoChar(&ddl, ')');
	for (i = nkeys; i < n; i++)
	{
		h->colnames[i] = get_attname(rte->relid, attnos[i], false);
		appendStringInfo(&ddl, "%s%s%s", i == nkeys ? " INCLUDE (" : ", ",
						 h->colnames[i], i == n - 1 ? ")" : "");
	}
	h->sql = ddl.data;			/* copy-pasteable DDL in the report */
	return h;
}

static void
advise_generate(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
				ProposedPathState * state)
{
	AdvCol		cols[PATHPROBE_ADVISE_MAX];
	int			ncols = 0;
	AttrNumber	comp[PATHPROBE_MAX_HYPO_COLS];
	int			ncomp = 0;
	const char *relname = get_rel_name(rte->relid);
	int			seq = list_length(state->hypo) + 1;
	ListCell   *lc;
	int			i,
				j;

	if (!relname)
		return;
	if (pg_class_aclcheck(rte->relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		return;					/* silently skip rels the user cannot read */

	/*
	 * Collect index atoms: equality atoms from EquivalenceClasses first
	 * (the planner's own dedup, with transitivity), then remaining shapes
	 * from baserestrictinfo.  Uniqueness: attno for columns, equal() for
	 * expression atoms.
	 */
	ncols = adv_atoms_from_ecs(root, rel, cols, ncols,
							   PATHPROBE_ADVISE_MAX);
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		AdvCol		c;

		if (ncols >= PATHPROBE_ADVISE_MAX)
			break;
		if (!adv_col_btree_qual(rel, rte, rinfo, &c))
			continue;
		for (i = 0; i < ncols; i++)
			if ((c.attno > 0 && cols[i].attno == c.attno) ||
				(c.attno == 0 && cols[i].attno == 0 &&
				 equal(cols[i].expr, c.expr)))
			{
				cols[i].is_eq |= c.is_eq;
				break;
			}
		if (i == ncols)
			cols[ncols++] = c;
	}

	/* transparency: the atoms stage is printable */
	{
		StringInfoData ab;

		initStringInfo(&ab);
		for (i = 0; i < ncols; i++)
		{
			if (i > 0)
				appendStringInfoString(&ab, ", ");
			if (cols[i].attno > 0)
				appendStringInfoString(&ab,
									   get_attname(rte->relid, cols[i].attno, false));
			else
				appendStringInfo(&ab, "(%s)",
								 deparse_expression((Node *) cols[i].expr,
													deparse_context_for(relname, rte->relid),
													false, false));
			appendStringInfoString(&ab, cols[i].is_eq ? "=" : "~");
		}
		if (state->atoms_str && state->atoms_str[0])
			state->atoms_str = psprintf("%s | %s: %s",
										state->atoms_str, relname, ab.data);
		else
			state->atoms_str = psprintf("%s: %s", relname, ab.data);
	}

	/* single-column candidates: equalities first */
	for (j = 0; j < 2; j++)		/* pass 0: eq, pass 1: range */
		for (i = 0; i < ncols; i++)
		{
			AttrNumber	a[1];

			if (cols[i].is_eq != (j == 0) || cols[i].attno == 0)
				continue;
			a[0] = cols[i].attno;
			if (adv_skip_candidate(state, rel, a, 1, 1))
				continue;
			if (adv_cap_reached(state))
				break;
			state->hypo = lappend(state->hypo,
								  adv_make_spec(rte, relname, a, 1, 0, seq++));
		}

	/* one composite: all equalities + first range column (btree shape) */
	for (i = 0; i < ncols && ncomp < PATHPROBE_MAX_HYPO_COLS; i++)
		if (cols[i].is_eq && cols[i].attno > 0)
			comp[ncomp++] = cols[i].attno;
	for (i = 0; i < ncols && ncomp < PATHPROBE_MAX_HYPO_COLS; i++)
		if (!cols[i].is_eq && cols[i].attno > 0)
		{
			comp[ncomp++] = cols[i].attno;
			break;
		}
	if (ncomp >= 2 && !adv_skip_candidate(state, rel, comp, ncomp, ncomp) &&
		!adv_cap_reached(state))
		state->hypo = lappend(state->hypo,
							  adv_make_spec(rte, relname, comp, ncomp, 0, seq++));


	/* expression atoms: single-column expression candidates via the real
	 * CREATE INDEX path (deparse -> parser -> transformIndexStmt), which
	 * already handles indexprs/indextlist.  Composite with expressions is
	 * out of scope for v1 (named boundary). */
	for (i = 0; i < ncols; i++)
	{
		char	   *edd;
		HypoIndexSpec *h;

		if (cols[i].attno != 0)
			continue;
		edd = deparse_expression((Node *) cols[i].expr,
								 deparse_context_for(relname, rte->relid),
								 false, false);
		if (adv_cap_reached(state))
			break;
		h = hypo_spec_from_sql(psprintf("CREATE INDEX adv_%d ON %s ((%s))",
										seq, relname, edd), -1);
		if (h != NULL)
		{
			h->name = psprintf("adv_%d", seq++);
			state->hypo = lappend(state->hypo, h);
		}
	}

	/* covering candidate: best key set + INCLUDE of remaining query columns */
	{
		AttrNumber	cov[PATHPROBE_MAX_HYPO_COLS];
		int			nkey = 0;
		int			ntot;
		ListCell   *lt;

		if (ncomp >= 2)
		{
			memcpy(cov, comp, sizeof(AttrNumber) * ncomp);
			nkey = ncomp;
		}
		else
		{
			for (i = 0; i < ncols; i++)
				if (cols[i].attno > 0)
				{
					cov[0] = cols[i].attno;
					nkey = 1;
					break;
				}
		}
		ntot = nkey;
		if (nkey > 0)
		{
			foreach(lt, rel->reltarget->exprs)
			{
				Node	   *e = (Node *) lfirst(lt);
				AttrNumber	a;
				int			k;

				if (!IsA(e, Var) || ((Var *) e)->varno != rel->relid ||
					((Var *) e)->varattno <= 0)
					continue;
				a = ((Var *) e)->varattno;
				for (k = 0; k < ntot; k++)
					if (cov[k] == a)
						break;
				if (k < ntot)
					continue;
				if (ntot >= PATHPROBE_MAX_HYPO_COLS)
				{
					ntot = nkey;	/* does not fit: no covering candidate */
					break;
				}
				cov[ntot++] = a;
			}
		}
		if (ntot > nkey && !adv_skip_candidate(state, rel, cov, ntot, nkey) &&
			!adv_cap_reached(state))
			state->hypo = lappend(state->hypo,
								  adv_make_spec(rte, relname, cov, ntot,
												nkey, seq++));
	}

	/* ordered candidate from query_pathkeys, else group_pathkeys (ASC-only) */
	{
		AttrNumber	ord[PATHPROBE_MAX_HYPO_COLS];
		int			nord = 0;
		List	   *pks = root->query_pathkeys != NIL ?
			root->query_pathkeys : root->group_pathkeys;
		bool		ok = (pks != NIL);

		foreach(lc, pks)
		{
			PathKey    *pk = (PathKey *) lfirst(lc);
			EquivalenceMember *em = NULL;
			ListCell   *lm;

			if (pk->pk_cmptype != COMPARE_LT || pk->pk_nulls_first)
			{
				ok = false;		/* DESC/NULLS FIRST needs the stmt form; v1 skips */
				break;
			}
			foreach(lm, pk->pk_eclass->ec_members)
			{
				EquivalenceMember *m = (EquivalenceMember *) lfirst(lm);

				if (IsA(m->em_expr, Var) &&
					((Var *) m->em_expr)->varno == rel->relid &&
					((Var *) m->em_expr)->varattno > 0)
				{
					em = m;
					break;
				}
			}
			if (!em || nord >= PATHPROBE_MAX_HYPO_COLS)
			{
				ok = false;
				break;
			}
			ord[nord++] = ((Var *) em->em_expr)->varattno;
		}
		if (ok && nord > 0 && !adv_skip_candidate(state, rel, ord, nord, nord) &&
			!adv_cap_reached(state))
			state->hypo = lappend(state->hypo,
								  adv_make_spec(rte, relname, ord, nord, 0, seq++));
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
	JsonbValue	key,
			   *arr;
	JsonbContainer *ac;
	int			n,
				i;

	key.type = jbvString;
	key.val.string.val = "hypo_indexes";
	key.val.string.len = strlen("hypo_indexes");
	arr = JB_ROOT_IS_OBJECT(spec_jb) ?
		findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &key) : NULL;
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
		JsonbValue	ck,
				   *cols;
		JsonbContainer *cc;
		int			ncols,
					c;

		if (!elem || elem->type != jbvBinary)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("pathprobe: hypo_indexes[%d] is not an object", i)));
		ejb = JsonbValueToJsonb(elem);

		h = (HypoIndexSpec *) palloc0(sizeof(HypoIndexSpec));
		{
			JsonbValue	rk;
			JsonbValue *rv;

			rk.type = jbvString;
			rk.val.string.val = "relname";
			rk.val.string.len = strlen("relname");
			rv = findJsonbValueFromContainer(&ejb->root, JB_FOBJECT, &rk);
			if (!rv || rv->type != jbvString)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("pathprobe: hypo_indexes[%d]: \"relname\" is required", i)));
			h->relname = pnstrdup(rv->val.string.val, rv->val.string.len);

			rk.val.string.val = "name";
			rk.val.string.len = strlen("name");
			rv = findJsonbValueFromContainer(&ejb->root, JB_FOBJECT, &rk);
			if (rv && rv->type == jbvString)
				h->name = pnstrdup(rv->val.string.val, rv->val.string.len);
			else
				h->name = psprintf("hypo%d", i + 1);
		}

		ck.type = jbvString;
		ck.val.string.val = "columns";
		ck.val.string.len = strlen("columns");
		cols = findJsonbValueFromContainer(&ejb->root, JB_FOBJECT, &ck);
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
	JsonbValue	key,
			   *arr;
	JsonbContainer *ac;
	int			n,
				i;

	key.type = jbvString;
	key.val.string.val = "hypo_sql";
	key.val.string.len = strlen("hypo_sql");
	arr = JB_ROOT_IS_OBJECT(spec_jb) ?
		findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &key) : NULL;
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
static HypoIndexSpec *
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
 */
static void
hypo_inject_one(PlannerInfo *root, RelOptInfo *rel,
				RangeTblEntry *rte, HypoIndexSpec * h)
{
	List	   *save = rel->indexlist;
	ListCell   *lc;

	h->ioi = hypo_build_indexoptinfo(root, rel, rte, h);

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
		advise_generate(root, rel, rte, state);

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
	JsonbValue	key,
			   *arr;

	key.type = jbvString;
	key.val.string.val = "candidates";
	key.val.string.len = strlen("candidates");

	arr = JB_ROOT_IS_OBJECT(spec_jb) ?
		findJsonbValueFromContainer(&spec_jb->root, JB_FOBJECT, &key) : NULL;

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
static PathProbeReport *
pathprobe_propose_internal(const char *query_string,
						   List *specs, List **hypo, bool advise,
						   bool advise_skip_prefix,
						   PathProbeOpts *opts,
						   PathProbeAccess *access)
{
	MemoryContext log_mcxt;
	PathEventLog *log;
	PathProbeReport *report;
	List	   *parsetree_list;
	ListCell   *lc;
	ListCell   *lc2;
	double		plan_cost = -1.0;
	ProposedPathState state;

	log_mcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "PathProbeProposedLog",
									 ALLOCSET_DEFAULT_SIZES);
	log = pelog_create(log_mcxt);

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
	active_proposal = &state;

	/* Install proposal inject hook (chains probe0) */
	prev_propose_rel_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = propose_inject_hook;

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
			List	   *plantree_list;

			{
				Oid		   *ptypes = NULL;
				int			nparams = 0;

				querytree_list = pg_analyze_and_rewrite_varparams(parsetree,
																  query_string,
																  &ptypes,
																  &nparams,
																  NULL);
			}

			/* ACL gate: authorize before any planning happens */
			pathprobe_authorize_query(querytree_list, access);

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
				}
			}

			if (querytree_list != NIL)
			{
				Query	   *q = linitial_node(Query, querytree_list);

				if (q->rtable)
					pelog_build_rtmap(log, q->rtable);
			}

			plantree_list = pg_plan_queries(querytree_list,
											query_string,
											CURSOR_OPT_PARALLEL_OK,
											NULL);
			if (plantree_list != NIL)
			{
				PlannedStmt *ps = linitial_node(PlannedStmt, plantree_list);

				if (ps->planTree)
				{
					plan_cost = ps->planTree->total_cost;
					propose_mark_in_plan(ps->planTree, state.hypo);
					foreach(lc2, ps->subplans)
						propose_mark_in_plan((Plan *) lfirst(lc2),
											 state.hypo);
				}
			}
		}
	}
	PG_CATCH();
	{
		set_rel_pathlist_hook = prev_propose_rel_hook;
		active_proposal = NULL;
		pathprobe_active_log = NULL;
		pathprobe_active_opts = NULL;
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Restore hooks */
	set_rel_pathlist_hook = prev_propose_rel_hook;
	active_proposal = NULL;
	*hypo = state.hypo;			/* advise mode may have generated entries */
	pathprobe_active_log = NULL;
	pathprobe_active_opts = NULL;

	report = MemoryContextAlloc(log_mcxt, sizeof(PathProbeReport));
	report->log = log;
	report->injected = state.injected;	/* for caller to check */
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

	return report;
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
		int			hint_nest = propose_apply_hints(spec_jb);

		pathprobe_log_accepted = true;
		report = pathprobe_propose_internal(query_str, specs, &hypo, advise,
											spec_get_bool(spec_jb, "advise_skip_prefix"),
											&opts, &access);
		pathprobe_log_accepted = saved_log_accepted;
		if (hypo != NIL || advise)
			report->baseline_cost = propose_baseline_cost(query_str);
		if (hint_nest > 0)
			AtEOXact_GUC(true, hint_nest);
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
			if (!h->injected)
			{
				appendStringInfo(&out, "%s  not injected (rel not in query)\n",
								 h->sql ? "]" : ")");
				continue;
			}
			{
				const char *closer = h->sql ? "]" : ")";
				int			acc = 0,
							rej = 0,
							surv = 0;
				bool		best_set = false;
				Cost		best = 0.0;
				char	   *tag = psprintf("hypo:%s", h->name);
				int			i;

				for (i = 0; i < report->log->nevents; i++)
				{
					PathEvent  *ev = &report->log->events[i];

					if (!ev->label || strcmp(ev->label, tag) != 0)
						continue;
					if (ev->kind == PEK_ACCEPTED)
						acc++;
					else if (ev->kind == PEK_REJECTED)
						rej++;
					else if (ev->kind == PEK_SURVIVED)
						surv++;
					if (!best_set || ev->cost_total < best)
					{
						best = ev->cost_total;
						best_set = true;
					}
				}
				appendStringInfo(&out, "%s  injected: acc=%d rej=%d surv=%d",
								 closer, acc, rej, surv);
				if (best_set)
					appendStringInfo(&out, "  best=%.2f", best);
				if (h->in_plan)
					appendStringInfoString(&out, "  IN PLAN");
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
 * Candidate fate as a finite-state machine over the labeled event stream
 * (seq order).  This applies to CustomPath candidates only: one path per
 * identity.  Hypothetical indexes keep aggregate counters - many paths
 * share one hypo label, a per-identity FSM is the wrong model there.
 *
 * States and legal transitions:
 *   INJECTED -accepted->            ACCEPTED
 *   INJECTED -rejected->            REJECTED     (terminal)
 *   ACCEPTED -survived->            SURVIVED     (frontier check)
 *   ACCEPTED/SURVIVED/UPPER -accepted @ upper rel-> UPPER
 * Anything else is an illegal transition: either the planner did something
 * we do not model, or our probes lie.  It is reported, not swallowed.
 */
typedef enum CandFateState
{
	CAND_INJECTED,
	CAND_ACCEPTED,
	CAND_REJECTED,
	CAND_SURVIVED,
	CAND_UPPER,
	CAND_ILLEGAL
}			CandFateState;

static CandFateState
propose_candidate_fsm(PathEventLog *log, ProposedPathSpec * spec,
					  int *illegal_seq)
{
	CandFateState st = CAND_INJECTED;
	int			i;

	*illegal_seq = -1;
	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];
		bool		is_upper;

		if (ev->source != PATH_SOURCE_PROPOSED ||
			!ev->label || strcmp(ev->label, spec->pathname) != 0)
			continue;
		is_upper = (ev->relids == NULL || bms_is_empty(ev->relids));

		switch (ev->kind)
		{
			case PEK_ACCEPTED:
				if (st == CAND_INJECTED && !is_upper)
					st = CAND_ACCEPTED;
				else if ((st == CAND_ACCEPTED || st == CAND_SURVIVED ||
						  st == CAND_UPPER) && is_upper)
					st = CAND_UPPER;
				else
					st = CAND_ILLEGAL;
				break;
			case PEK_REJECTED:
				st = (st == CAND_INJECTED) ? CAND_REJECTED : CAND_ILLEGAL;
				break;
			case PEK_SURVIVED:
				st = (st == CAND_ACCEPTED) ? CAND_SURVIVED : CAND_ILLEGAL;
				break;
			default:
				st = CAND_ILLEGAL;
				break;
		}
		if (st == CAND_ILLEGAL)
		{
			*illegal_seq = ev->seq;
			return st;
		}
	}
	return st;
}

static const char *
cand_fate_text(CandFateState st, bool injected, int illegal_seq,
			   bool json_form)
{
	if (!injected)
		return json_form ? "not_injected" : "not injected";
	switch (st)
	{
		case CAND_SURVIVED:
			return "survived";
		case CAND_UPPER:
			return json_form ? "survived_upper" : "survived (reached upper rel)";
		case CAND_ACCEPTED:
			return json_form ? "accepted_not_survived" : "accepted -> not survived";
		case CAND_REJECTED:
			return "rejected";
		case CAND_ILLEGAL:
			return json_form ? "inconsistent" : "INCONSISTENT (illegal transition)";
		case CAND_INJECTED:
		default:
			return "unknown";
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
	bool		wp_accepted = false;
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
			if (ev->kind == PEK_ACCEPTED)
				wp_accepted = true;
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
	{
		int			illegal_seq;
		CandFateState st = propose_candidate_fsm(log, spec, &illegal_seq);

		fate = cand_fate_text(st, spec->injected, illegal_seq, false);
		if (st == CAND_ILLEGAL)
			note = psprintf("event #%d broke the fate machine: "
							"either an unmodeled planner action or a probe bug",
							illegal_seq);
	}

	/*
	 * gap = candidate_best_cost - winner_cost < 0: candidate would win on raw
	 * cost > 0: candidate genuinely lost
	 */
	gap_valid = (wp_cost_set && winner_set);
	gap = gap_valid ? (wp_best_cost - winner_cost) : 0.0;

	/* Policy-aware note */
	if (spec->winner_forbidden && wp_accepted && !wp_survived)
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
		/* keep text output consistent with JSON fate "not_injected" */
		appendStringInfoString(out,
							   "fate   : not injected "
							   "(target relation not found in query)\n");
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
				n_surv = 0;
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
		CandFateState st = propose_candidate_fsm(log, spec, &illegal_seq);

		fate = cand_fate_text(st, spec->injected, illegal_seq, true);
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
					 ",\"event_counts\":{\"accepted\":%d,\"rejected\":%d,\"survived\":%d}",
					 n_acc, n_rej, n_surv);
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
		int			hint_nest = propose_apply_hints(spec_jb);

		pathprobe_log_accepted = true;
		report = pathprobe_propose_internal(query_str, specs, &hypo, advise,
											spec_get_bool(spec_jb, "advise_skip_prefix"),
											&opts, &access);
		pathprobe_log_accepted = saved_log_accepted;
		if (hypo != NIL || advise)
			report->baseline_cost = propose_baseline_cost(query_str);
		if (hint_nest > 0)
			AtEOXact_GUC(true, hint_nest);
	}

	/* Retag wp events */
	retag_proposed_events(report->log);

	if (spec == NULL && specs == NIL)
	{
		/* hypo-only call: no legacy candidate block; emit events+candidates */
		spec = (ProposedPathSpec *) palloc0(sizeof(ProposedPathSpec));
		spec->pathname = "";
		spec->relname = "";
		spec->provider = "";
	}

	/* Derive fate (same logic as render_propose_card) */
	{
		PathEventLog *log = report->log;
		int			i;
		bool		wp_accepted = false;
		bool		wp_survived = false;
		bool		wp_rejected = false;
		Cost		wp_best_cost = 0.0;
		bool		wp_cost_set = false;
		NodeTag		winner_type = T_Invalid;
		Cost		winner_cost = 0.0;
		bool		winner_set = false;
		const char *fate;
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
				if (ev->kind == PEK_ACCEPTED)
					wp_accepted = true;
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

		if (!spec->injected)
			fate = "not_injected";
		else if (wp_survived)
			fate = "survived";
		else if (wp_accepted)
			fate = "accepted_not_survived";
		else if (wp_rejected)
			fate = "rejected";
		else
			fate = "unknown";

		gap_valid = (wp_cost_set && winner_set);
		gap = gap_valid ? (wp_best_cost - winner_cost) : 0.0;

		if (spec->winner_forbidden && wp_accepted && !wp_survived)
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
		appendStringInfo(&out,
						 "  \"candidate\": {"
						 "\"pathname\":\"%s\","
						 "\"provider\":\"%s\","
						 "\"cost_total\":%.2f,"
						 "\"rows\":%.0f,"
						 "\"winner_forbidden\":%s"
						 "},\n",
						 spec->pathname, spec->provider,
						 spec->total_cost, spec->rows,
						 spec->winner_forbidden ? "true" : "false");

		/* target block */
		appendStringInfo(&out,
						 "  \"target\": {\"relname\":\"%s\"},\n",
						 spec->relname);

		/* fate block */
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
				char	   *hlabel = psprintf("hypo:%s", h->name);
				int			ha = 0,
							hr = 0,
							hs = 0;
				Cost		hbest = 0.0;
				bool		hbest_set = false;
				int			ei;

				for (ei = 0; ei < report->log->nevents; ei++)
				{
					PathEvent  *ev = &report->log->events[ei];

					if (!ev->label || strcmp(ev->label, hlabel) != 0)
						continue;
					if (ev->kind == PEK_ACCEPTED)
						ha++;
					else if (ev->kind == PEK_REJECTED)
						hr++;
					else if (ev->kind == PEK_SURVIVED)
						hs++;
					if (!hbest_set || ev->cost_total < hbest)
					{
						hbest = ev->cost_total;
						hbest_set = true;
					}
				}
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
								 ",\"accepted\":%d,\"rejected\":%d,\"survived\":%d",
								 ha, hr, hs);
				if (hbest_set)
					appendStringInfo(&out, ",\"best_cost\":%.2f", hbest);
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
