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
 * The patch exposes two hooks in pathnode.c / pathnode.h:
 *
 *   pathprobe_path_rejected_hook(rel, new_path, dominated_by)
 *   pathprobe_path_accepted_hook(rel, new_path, dominated_list)
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
 * probe1_rejected_hook
 *
 * Called by add_path() when new_path is dominated.
 * dominated_by is parent_rel->cheapest_total_path at call time.  NOTE:
 * set_cheapest() has not yet run while add_path() is populating the rel
 * (allpaths.c:607, joinrels.c:1538), so in practice dominated_by is NULL
 * and gap stays 0; the min_gap filter below does not apply to REJECTED
 * until the core patch passes the actual dominating old_path.
 *
 * Note: new_path may be pfree'd immediately after this call.
 * We snapshot all needed fields before returning.
 */
void
probe1_rejected_hook(RelOptInfo *rel, Path *new_path, Path *dominated_by)
{
	PathEvent	ev;
	double		gap;

	if (pathprobe_active_log == NULL)
		return;

	/*
	 * gap for REJECTED: how much more expensive new_path is vs the winner.
	 * Positive = new_path lost (normal case). Negative = new_path lost on
	 * startup cost, not total.
	 */
	gap = dominated_by ? (new_path->total_cost - dominated_by->total_cost)
		: 0.0;

	/* Apply min_gap filter */
	if (pathprobe_min_gap > 0.0 && fabs(gap) < pathprobe_min_gap)
		return;

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
	ev.other_pathtype = dominated_by ? dominated_by->pathtype : T_Invalid;

	{
		const char *pname = pathprobe_path_label(new_path);

		if (pname)
			ev.label = MemoryContextStrdup(pathprobe_active_log->mcxt, pname);
	}
	pelog_append(pathprobe_active_log, &ev);
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
		return;

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
	pelog_append(pathprobe_active_log, &ev);
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
