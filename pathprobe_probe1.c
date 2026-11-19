/*-------------------------------------------------------------------------
 * pathprobe_probe1.c
 * Part of pathprobe.  Distributed under the PostgreSQL License;
 * see the repository LICENSE file.
 *-------------------------------------------------------------------------
 */
/*
 * pathprobe_probe1.c - add_path competition adapter for pathprobe
 *
 * Requires: pathprobe_add_path.patch applied to PostgreSQL master.
 *
 * The patch exposes these hooks in pathnode.c / pathnode.h:
 *
 *   pathprobe_path_rejected_hook(rel, new_path, dominated_by)
 *   pathprobe_path_accepted_hook(rel, new_path, dominated_list)
 *   pathprobe_path_removed_hook(rel, new_path, removed_list)
 *
 * This adapter writes events to pathprobe_active_log when active.
 * kind=PEK_REJECTED / PEK_ACCEPTED, source=PATH_SOURCE_ADD_PATH.
 *
 * One-time seq assignment happens in pelog_append() - not here.
 * This adapter only fills PathEvent fields and calls pelog_append().
 *
 * GUCs:
 *   pathprobe.log_accepted   bool  default off  (high volume: O(paths?))
 *   pathprobe.min_gap        float default 0.0  (skip if |gap| < N)
 */

#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "optimizer/pathnode.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include <math.h>

#include "utils/memutils.h"
#include "pathprobe.h"

/* GUCs (registered by pathprobe main _PG_init, shared via extern) */
extern bool pathprobe_log_accepted;
extern double pathprobe_min_gap;



/*
 * The last path the rejected hook saw, with the id and seq of its REJECTED
 * event (0 if none was recorded).  add_path() calls the removed hook right
 * after the rejected hook for the same new_path, which by then is no
 * longer in the id map; the removed hook links to it through this.
 */
static const Path *last_rejected = NULL;
static int	last_rejected_id = 0;
static int	last_rejected_seq = 0;

/* STD_FUZZ_FACTOR and PathCostComparison are local to pathnode.c */
#define PP_STD_FUZZ_FACTOR 1.01

typedef enum
{
	PP_COSTS_EQUAL,
	PP_COSTS_BETTER1,
	PP_COSTS_BETTER2,
	PP_COSTS_DIFFERENT,
} PpCostComparison;

/*
 * Copy of compare_path_costs_fuzzily() (static in pathnode.c), which also
 * says whether disabled_nodes decided.  It must stay identical to the core
 * function: probe1_decided_by() reports DECIDED_UNKNOWN when the two
 * disagree on the winner, but a drift that keeps the winner and changes
 * the reason goes unnoticed.
 */
static PpCostComparison
pp_compare_costs_fuzzily(Path *path1, Path *path2, double fuzz_factor,
						 bool *by_disabled)
{
#define PP_CONSIDER_STARTUP(p) \
	((p)->param_info == NULL ? (p)->parent->consider_startup : \
	 (p)->parent->consider_param_startup)

	*by_disabled = false;
	if (path1->disabled_nodes != path2->disabled_nodes)
	{
		*by_disabled = true;
		return path1->disabled_nodes < path2->disabled_nodes ?
			PP_COSTS_BETTER1 : PP_COSTS_BETTER2;
	}
	if (path1->total_cost > path2->total_cost * fuzz_factor)
	{
		if (PP_CONSIDER_STARTUP(path1) &&
			path2->startup_cost > path1->startup_cost * fuzz_factor)
			return PP_COSTS_DIFFERENT;
		return PP_COSTS_BETTER2;
	}
	if (path2->total_cost > path1->total_cost * fuzz_factor)
	{
		if (PP_CONSIDER_STARTUP(path2) &&
			path1->startup_cost > path2->startup_cost * fuzz_factor)
			return PP_COSTS_DIFFERENT;
		return PP_COSTS_BETTER1;
	}
	if (path1->startup_cost > path2->startup_cost * fuzz_factor)
		return PP_COSTS_BETTER2;
	if (path2->startup_cost > path1->startup_cost * fuzz_factor)
		return PP_COSTS_BETTER1;
	return PP_COSTS_EQUAL;
#undef PP_CONSIDER_STARTUP
}

/*
 * probe1_decided_by
 *
 * Repeat add_path()'s comparison of new_path with old_path and return the
 * first test, in add_path()'s order, that decided it.  new_won says what
 * add_path() decided: true when new_path removed old_path (DISPLACED,
 * REMOVED), false when old_path dominated new_path (REJECTED).  Both paths
 * are still allocated when the hooks run.  The later tests a winner only
 * has to pass without being worse (rows, parallel safety, outer rels) are
 * not reported.
 */
static PathDecidedBy
probe1_decided_by(Path *new_path, Path *old_path, bool new_won)
{
	List	   *new_keys = new_path->param_info ? NIL : new_path->pathkeys;
	List	   *old_keys = old_path->param_info ? NIL : old_path->pathkeys;
	Relids		new_outer = PATH_REQ_OUTER(new_path);
	Relids		old_outer = PATH_REQ_OUTER(old_path);
	bool		new_le_rows = new_path->rows <= old_path->rows;
	bool		new_ge_rows = new_path->rows >= old_path->rows;
	bool		new_ge_ps = new_path->parallel_safe >= old_path->parallel_safe;
	bool		new_le_ps = new_path->parallel_safe <= old_path->parallel_safe;
	PpCostComparison costcmp;
	PathKeysComparison keyscmp;
	BMS_Comparison outercmp;
	bool		by_disabled;
	int			winner = 0;		/* 1 new, 2 old, 0 neither */
	PathDecidedBy by = DECIDED_UNKNOWN;

	costcmp = pp_compare_costs_fuzzily(new_path, old_path,
									   PP_STD_FUZZ_FACTOR, &by_disabled);
	if (costcmp == PP_COSTS_DIFFERENT)
		return DECIDED_UNKNOWN;
	keyscmp = compare_pathkeys(new_keys, old_keys);
	if (keyscmp == PATHKEYS_DIFFERENT)
		return DECIDED_UNKNOWN;
	outercmp = bms_subset_compare(new_outer, old_outer);

	switch (costcmp)
	{
		case PP_COSTS_BETTER1:
			if (keyscmp != PATHKEYS_BETTER2 &&
				(outercmp == BMS_EQUAL || outercmp == BMS_SUBSET1) &&
				new_le_rows && new_ge_ps)
				winner = 1;
			by = by_disabled ? DECIDED_DISABLED_NODES : DECIDED_COST;
			break;
		case PP_COSTS_BETTER2:
			if (keyscmp != PATHKEYS_BETTER1 &&
				(outercmp == BMS_EQUAL || outercmp == BMS_SUBSET2) &&
				new_ge_rows && new_le_ps)
				winner = 2;
			by = by_disabled ? DECIDED_DISABLED_NODES : DECIDED_COST;
			break;
		case PP_COSTS_EQUAL:
			if (keyscmp == PATHKEYS_BETTER1)
			{
				if ((outercmp == BMS_EQUAL || outercmp == BMS_SUBSET1) &&
					new_le_rows && new_ge_ps)
					winner = 1;
				by = DECIDED_PATHKEYS;
			}
			else if (keyscmp == PATHKEYS_BETTER2)
			{
				if ((outercmp == BMS_EQUAL || outercmp == BMS_SUBSET2) &&
					new_ge_rows && new_le_ps)
					winner = 2;
				by = DECIDED_PATHKEYS;
			}
			else if (outercmp == BMS_EQUAL)
			{
				if (new_path->parallel_safe != old_path->parallel_safe)
				{
					winner = new_path->parallel_safe ? 1 : 2;
					by = DECIDED_PARALLEL_SAFE;
				}
				else if (new_path->rows != old_path->rows)
				{
					winner = new_path->rows < old_path->rows ? 1 : 2;
					by = DECIDED_ROWS;
				}
				else
				{
					/*
					 * add_path() keeps old_path unless this says BETTER1;
					 * DIFFERENT here is a tie too
					 */
					costcmp = pp_compare_costs_fuzzily(new_path, old_path,
													   1.0000000001,
													   &by_disabled);
					winner = costcmp == PP_COSTS_BETTER1 ? 1 : 2;
					by = (costcmp == PP_COSTS_BETTER1 ||
						  costcmp == PP_COSTS_BETTER2) ?
						DECIDED_EXACT_COST : DECIDED_TIE;
				}
			}
			else
			{
				if (outercmp == BMS_SUBSET1 && new_le_rows && new_ge_ps)
					winner = 1;
				else if (outercmp == BMS_SUBSET2 && new_ge_rows && new_le_ps)
					winner = 2;
				by = DECIDED_REQUIRED_OUTER;
			}
			break;
		case PP_COSTS_DIFFERENT:
			break;
	}

	if (winner != (new_won ? 1 : 2))
		return DECIDED_UNKNOWN;
	return by;
}

const char *
decided_by_name(int decided_by)
{
	switch ((PathDecidedBy) decided_by)
	{
		case DECIDED_NONE:
			return NULL;
		case DECIDED_DISABLED_NODES:
			return "disabled_nodes";
		case DECIDED_COST:
			return "cost";
		case DECIDED_PATHKEYS:
			return "pathkeys";
		case DECIDED_REQUIRED_OUTER:
			return "required_outer";
		case DECIDED_PARALLEL_SAFE:
			return "parallel_safe";
		case DECIDED_ROWS:
			return "rows";
		case DECIDED_EXACT_COST:
			return "exact_cost";
		case DECIDED_TIE:
			return "tie";
		case DECIDED_UNKNOWN:
			break;
	}
	return "unknown";
}

/*
 * probe1_rejected_hook
 *
 * Called by add_path() when new_path is dominated.  With
 * HAVE_PATHPROBE_REJECTED_DOMINATOR, dominated_by is the old path that
 * add_path() found to dominate new_path; it is recorded as the event's
 * link_*.  Older copies of the core patch pass rel->cheapest_total_path
 * instead, which is not recorded as a dominator.
 *
 * gap_cost, other_pathtype and the min_gap filter keep using
 * rel->cheapest_total_path, as before.  set_cheapest() has not yet run
 * while add_path() is populating the rel (allpaths.c:607,
 * joinrels.c:1538), so in practice that is NULL and gap stays 0.
 *
 * Note: new_path may be pfree'd immediately after this call.
 * We snapshot all needed fields before returning.
 */
void
probe1_rejected_hook(RelOptInfo *rel, Path *new_path, Path *dominated_by)
{
	PathEvent	ev;
	double		gap;
	Path	   *proxy = rel->cheapest_total_path;
#ifdef HAVE_PATHPROBE_REJECTED_DOMINATOR
	Path	   *dominator = dominated_by;
#else
	Path	   *dominator = NULL;
#endif

	if (pathprobe_active_log == NULL)
		return;

	last_rejected = new_path;
	last_rejected_id = 0;
	last_rejected_seq = 0;

	/*
	 * gap for REJECTED: how much more expensive new_path is vs the winner.
	 * Positive = new_path lost (normal case). Negative = new_path lost on
	 * startup cost, not total.
	 */
	gap = proxy ? (new_path->total_cost - proxy->total_cost) : 0.0;

	/* Apply min_gap filter */
	if (pathprobe_min_gap > 0.0 && fabs(gap) < pathprobe_min_gap)
	{
		if (!IsA(new_path, IndexPath))
			pelog_path_forget(pathprobe_active_log, new_path);
		return;
	}

	memset(&ev, 0, sizeof(ev));

	/* Relation identity */
	ev.relid = InvalidOid;
	ev.relids = pelog_intern_relids(pathprobe_active_log, rel->relids);

	/* Path fields - snapshot before pfree */
	ev.pathtype = new_path->pathtype;
	ev.cost_startup = new_path->startup_cost;
	ev.cost_total = new_path->total_cost;
	ev.disabled_nodes = new_path->disabled_nodes;

	/* Event metadata */
	ev.kind = PEK_REJECTED;
	ev.origin = PATH_ORIGIN_REAL;
	ev.source = PATH_SOURCE_ADD_PATH;

	/* Competition context */
	ev.is_parameterized = (new_path->param_info != NULL);
	ev.gap_cost = gap;
	ev.other_pathtype = proxy ? proxy->pathtype : T_Invalid;

	{
		const char *pname = pathprobe_path_label(new_path);

		if (pname)
			ev.label = MemoryContextStrdup(pathprobe_active_log->mcxt, pname);
	}
	ev.path_id = pelog_path_id(pathprobe_active_log, new_path);
	if (dominator)
	{
		ev.link_path_id = pelog_path_id(pathprobe_active_log, dominator);
		ev.link_seq = pelog_path_accepted_seq(pathprobe_active_log,
											  dominator, rel);
		ev.link_pathtype = dominator->pathtype;
		ev.decided_by = probe1_decided_by(new_path, dominator, false);
	}
	pelog_append(pathprobe_active_log, &ev);	/* sets ev.seq, or 0 if full */
	last_rejected_id = ev.path_id;
	last_rejected_seq = ev.seq;

	/* add_path() frees a rejected path unless it is an IndexPath */
	if (!IsA(new_path, IndexPath))
		pelog_path_forget(pathprobe_active_log, new_path);
}

/*
 * probe1_accepted_hook
 *
 * Called by add_path() when new_path survives.
 * dominated_list: paths that new_path displaced (may be NIL).
 *
 * Controlled by pathprobe.log_accepted (default off) - high volume.
 */
void
probe1_accepted_hook(RelOptInfo *rel, Path *new_path, List *dominated_list)
{
	PathEvent	ev;

	if (pathprobe_active_log == NULL)
		return;

	if (!pathprobe_log_accepted)
	{
		/* not recorded, but add_path() frees them: forget their ids */
		foreach_ptr(Path, old_path, dominated_list)
		{
			if (!IsA(old_path, IndexPath))
				pelog_path_forget(pathprobe_active_log, old_path);
		}
		return;
	}

	memset(&ev, 0, sizeof(ev));

	ev.relids = pelog_intern_relids(pathprobe_active_log, rel->relids);
	ev.relid = InvalidOid;
	ev.pathtype = new_path->pathtype;
	ev.cost_startup = new_path->startup_cost;
	ev.cost_total = new_path->total_cost;
	ev.disabled_nodes = new_path->disabled_nodes;

	ev.kind = PEK_ACCEPTED;
	ev.origin = PATH_ORIGIN_REAL;
	ev.source = PATH_SOURCE_ADD_PATH;
	ev.is_parameterized = (new_path->param_info != NULL);

	/* gap = how much cheaper than best displaced path (if any) */
	if (dominated_list != NIL)
	{
		Path	   *worst = (Path *) linitial(dominated_list);

		/* improvement = how much cheaper new_path is vs best displaced */
		ev.gap_cost = worst->total_cost - new_path->total_cost;
		ev.other_pathtype = worst->pathtype;
	}

	{
		const char *pname = pathprobe_path_label(new_path);

		if (pname)
			ev.label = MemoryContextStrdup(pathprobe_active_log->mcxt, pname);
	}
	ev.path_id = pelog_path_id(pathprobe_active_log, new_path);
	pelog_append(pathprobe_active_log, &ev);	/* sets ev.seq, or 0 if full */
	pelog_path_accepted(pathprobe_active_log, new_path, rel, ev.seq);

	/*
	 * One DISPLACED event per path that new_path removed from the pathlist.
	 * The core patch keeps these paths allocated until this hook returns.
	 */
	foreach_ptr(Path, old_path, dominated_list)
	{
		PathEvent	dev;
		const char *dname = pathprobe_path_label(old_path);

		memset(&dev, 0, sizeof(dev));
		dev.relids = ev.relids;
		dev.relid = InvalidOid;
		dev.pathtype = old_path->pathtype;
		dev.cost_startup = old_path->startup_cost;
		dev.cost_total = old_path->total_cost;
		dev.disabled_nodes = old_path->disabled_nodes;
		dev.kind = PEK_DISPLACED;
		dev.origin = PATH_ORIGIN_REAL;
		dev.source = PATH_SOURCE_ADD_PATH;
		dev.is_parameterized = (old_path->param_info != NULL);
		dev.gap_cost = old_path->total_cost - new_path->total_cost;
		dev.other_pathtype = new_path->pathtype;
		if (dname)
			dev.label = MemoryContextStrdup(pathprobe_active_log->mcxt, dname);
		dev.path_id = pelog_path_id(pathprobe_active_log, old_path);
		dev.link_path_id = ev.path_id;
		dev.link_seq = ev.seq;
		dev.link_pathtype = new_path->pathtype;
		dev.decided_by = probe1_decided_by(new_path, old_path, true);
		pelog_append(pathprobe_active_log, &dev);

		/* freed by add_path() after this hook, unless an IndexPath */
		if (!IsA(old_path, IndexPath))
			pelog_path_forget(pathprobe_active_log, old_path);
	}
}

/*
 * probe1_removed_hook
 *
 * Called by add_path() after the rejected hook, when new_path removed old
 * paths from the pathlist before an old path was found to dominate it.
 * Each removed path gets a REMOVED event, not DISPLACED: its remover never
 * entered the pathlist, so it has a REJECTED event, not an ACCEPTED one.
 * The event links to the remover and its REJECTED event.  add_path() frees
 * the removed paths (except IndexPaths) when this returns, so their ids are
 * dropped.  Recorded whether or not acceptance is logged.
 */
void
probe1_removed_hook(RelOptInfo *rel, Path *new_path, List *removed_list)
{
	PathEventLog *log = pathprobe_active_log;
	int			remover_id = 0;
	int			remover_seq = 0;
	bool		remover_registered = false;
	Relids		relids;

	if (log == NULL)
		return;

	if (last_rejected == new_path)
	{
		remover_id = last_rejected_id;
		remover_seq = last_rejected_seq;
	}
	last_rejected = NULL;
	if (remover_id == 0)
	{
		/* no REJECTED event (min_gap): new_path still gets an id */
		remover_id = pelog_path_id(log, new_path);
		remover_registered = true;
	}

	relids = pelog_intern_relids(log, rel->relids);
	foreach_ptr(Path, old_path, removed_list)
	{
		PathEvent	ev;
		const char *name = pathprobe_path_label(old_path);

		memset(&ev, 0, sizeof(ev));
		ev.relids = relids;
		ev.relid = InvalidOid;
		ev.pathtype = old_path->pathtype;
		ev.cost_startup = old_path->startup_cost;
		ev.cost_total = old_path->total_cost;
		ev.disabled_nodes = old_path->disabled_nodes;
		ev.kind = PEK_REMOVED;
		ev.origin = PATH_ORIGIN_REAL;
		ev.source = PATH_SOURCE_ADD_PATH;
		ev.is_parameterized = (old_path->param_info != NULL);
		ev.other_pathtype = new_path->pathtype;
		if (name)
			ev.label = MemoryContextStrdup(log->mcxt, name);
		ev.path_id = pelog_path_id(log, old_path);
		ev.link_path_id = remover_id;
		ev.link_seq = remover_seq;
		ev.link_pathtype = new_path->pathtype;
		ev.decided_by = probe1_decided_by(new_path, old_path, true);
		pelog_append(log, &ev);

		if (!IsA(old_path, IndexPath))
			pelog_path_forget(log, old_path);
	}

	/* the rejected hook already forgot new_path; undo our registration */
	if (remover_registered && !IsA(new_path, IndexPath))
		pelog_path_forget(log, new_path);
}

/*
 * probe_skipped_hook
 *
 * Called from try_nestloop_path / try_mergejoin_path / try_hashjoin_path
 * when add_path_precheck() returns false.
 *
 * The join path was never constructed - this is the "neverborn" event.
 * We record its cost lower bound and join strategy type.
 *
 * Note: no Path struct exists yet. We record what we know from workspace
 * and the hook arguments. relids come from joinrel->relids.
 */
void
probe_skipped_hook(RelOptInfo *joinrel, NodeTag join_pathtype,
				   int disabled_nodes,
				   Cost startup_cost, Cost total_cost,
				   Relids required_outer)
{
	PathEvent	ev;

	if (pathprobe_active_log == NULL)
		return;

	memset(&ev, 0, sizeof(ev));

	ev.relid = InvalidOid;
	ev.relids = bms_copy(joinrel->relids);
	ev.pathtype = join_pathtype;	/* T_NestLoop / T_MergeJoin / T_HashJoin */
	ev.kind = PEK_SKIPPED;
	ev.origin = PATH_ORIGIN_REAL;
	ev.source = PATH_SOURCE_PRECHECK;
	ev.is_parameterized = (required_outer != NULL && !bms_is_empty(required_outer));
	ev.cost_startup = startup_cost;
	ev.cost_total = total_cost;
	ev.disabled_nodes = disabled_nodes;

	/*
	 * Distinguish skip reason: - disabled_nodes > 0: join method disabled via
	 * GUC (enable_nestloop=off etc) - disabled_nodes == 0: cost lower bound
	 * dominated existing path
	 *
	 * disabled_nodes is set by initial_cost_*() from pgs_mask vs enable_mask.
	 * It is non-zero whenever a disabled plan node is in the path.
	 */
	ev.skip_reason = (disabled_nodes > 0) ? SKIP_DISABLED : SKIP_DOMINATED_EARLY;

	pelog_append(pathprobe_active_log, &ev);
}
