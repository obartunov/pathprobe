/*-------------------------------------------------------------------------
 * pathprobe_probe2.c
 * Part of pathprobe.  Distributed under the PostgreSQL License;
 * see the repository LICENSE file.
 *-------------------------------------------------------------------------
 */
/*
 * pathprobe_probe2.c - join-level hook adapter (package v1.2)
 *
 * Three hooks implemented here:
 *
 * probe2_join_setup_hook  (join_path_setup_hook)
 *   Fires at start of add_paths_to_joinrel() for each (outer, inner) pair.
 *   Records the join families that this pair's pgs_mask keeps
 *   add_paths_to_joinrel() from trying at all: merge and hash join, except
 *   for a full join.  A cleared PGS_NESTLOOP_PLAIN only disables the plain
 *   nested loop, which is still built, so NestLoop is never reported here.
 *   The event names the join relation, not the pair: a hook that sets the
 *   mask per pair can leave a family excluded for one input order and tried
 *   for the other.
 *   kind=PEK_EXCLUDED, source=PATH_SOURCE_JOIN_HOOK.
 *
 * probe2_join_pathlist_hook  (set_join_pathlist_hook)
 *   Fires at end of add_paths_to_joinrel() after all paths for the pair
 *   have been added and pruned.  Records survivors from joinrel->pathlist.
 *   kind=PEK_SURVIVED, source=PATH_SOURCE_JOIN_HOOK.
 *   This is the join-level analogue of probe0 for base rels.
 *
 * probe2_join_search_hook  (join_search_hook, secondary source j2)
 *   Wraps standard_join_search().  Records a summary event per join level
 *   after all pairs at that level are processed.
 *   kind=PEK_SURVIVED, source=PATH_SOURCE_JOIN_HOOK.
 *   (v0.4: lightweight, one event per nrels level)
 *
 * All three write to pathprobe_active_log with the same seq counter.
 */

#include "postgres.h"

#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "pathprobe.h"

/* saved previous hooks */
static join_path_setup_hook_type prev_join_path_setup_hook = NULL;
static set_join_pathlist_hook_type prev_set_join_pathlist_hook = NULL;
static join_search_hook_type prev_join_search_hook = NULL;

/* forward declarations */
void		probe2_join_setup_hook(PlannerInfo *root, RelOptInfo *joinrel,
								   RelOptInfo *outerrel, RelOptInfo *innerrel,
								   JoinType jointype, JoinPathExtraData *extra);
void		probe2_join_pathlist_hook(PlannerInfo *root, RelOptInfo *joinrel,
									  RelOptInfo *outerrel, RelOptInfo *innerrel,
									  JoinType jointype, JoinPathExtraData *extra);
RelOptInfo *probe2_join_search_hook(PlannerInfo *root, int levels_needed,
									List *initial_rels);

/* -------------------------------------------------------------------------
 * probe2_join_setup_hook
 *
 * join_path_setup_hook: fires before any path generation for this pair.
 * Records PEK_EXCLUDED events for the join families that
 * add_paths_to_joinrel() will not try for this pair.
 * -------------------------------------------------------------------------
 */
void
probe2_join_setup_hook(PlannerInfo *root, RelOptInfo *joinrel,
					   RelOptInfo *outerrel, RelOptInfo *innerrel,
					   JoinType jointype, JoinPathExtraData *extra)
{
	PathEvent	ev;
	uint64		mask;

	/* chain first */
	if (prev_join_path_setup_hook)
		prev_join_path_setup_hook(root, joinrel, outerrel, innerrel,
								  jointype, extra);

	if (!pathprobe_enable_probe2 || pathprobe_active_log == NULL)
		return;

	mask = extra->pgs_mask;

	/*
	 * One EXCLUDED event per join family that is not tried for this pair.
	 * Merge and hash join are skipped when all their bits are clear, unless
	 * the join is a full join: then they may be the only way to implement it
	 * and are tried as disabled paths, where the clauses allow.  A plain
	 * nested loop is tried whenever the join type allows one; a cleared
	 * PGS_NESTLOOP_PLAIN only marks it disabled (initial_cost_nestloop).
	 * The memoize and materialize variants are not built when their bits
	 * are clear, but the family as a whole is never excluded.
	 */

#define EMIT_EXCLUDED(join_pathtype, pgs_bits)                              \
    if ((mask & (pgs_bits)) == 0)                                           \
    {                                                                       \
        memset(&ev, 0, sizeof(ev));                                         \
        ev.relids           = bms_copy(joinrel->relids);                    \
        ev.relid            = InvalidOid;                                   \
        ev.pathtype         = (join_pathtype);                              \
        ev.kind             = PEK_EXCLUDED;                                 \
        ev.origin           = PATH_ORIGIN_REAL;                             \
        ev.source           = PATH_SOURCE_JOIN_HOOK;                        \
        ev.is_parameterized = false;                                        \
        pelog_append(pathprobe_active_log, &ev);                            \
    }

	if (jointype != JOIN_FULL)
	{
		EMIT_EXCLUDED(T_MergeJoin, PGS_MERGEJOIN_ANY)
		EMIT_EXCLUDED(T_HashJoin, PGS_HASHJOIN)
	}

#undef EMIT_EXCLUDED
}

/* -------------------------------------------------------------------------
 * probe2_join_pathlist_hook
 *
 * set_join_pathlist_hook: fires after all paths for (outer, inner) pair
 * have been generated and add_path()-pruned.
 * Records PEK_SURVIVED events from joinrel->pathlist - join-level frontier.
 * -------------------------------------------------------------------------
 */
void
probe2_join_pathlist_hook(PlannerInfo *root, RelOptInfo *joinrel,
						  RelOptInfo *outerrel, RelOptInfo *innerrel,
						  JoinType jointype, JoinPathExtraData *extra)
{
	ListCell   *lc;
	PathEvent	ev;
	Path	   *cheap_total = NULL;
	Path	   *cheap_startup = NULL;

	/* chain first */
	if (prev_set_join_pathlist_hook)
		prev_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
									jointype, extra);

	if (!pathprobe_enable_probe2)
		return;

	if (pathprobe_active_log == NULL)
		return;

	/*
	 * Reverse case (join level): set_cheapest() has not run yet, so derive
	 * cheapest_total / cheapest_startup from the frontier to mark survivors
	 * kept for a non-cheapest reason (e.g. a MergeJoin kept for its order).
	 */
	foreach(lc, joinrel->pathlist)
	{
		Path	   *p = (Path *) lfirst(lc);

		if (p->param_info != NULL)
			continue;
		if (cheap_total == NULL || p->total_cost < cheap_total->total_cost)
			cheap_total = p;
		if (cheap_startup == NULL || p->startup_cost < cheap_startup->startup_cost)
			cheap_startup = p;
	}

	/*
	 * Pair-local frontier snapshot. Records joinrel->pathlist after this
	 * outerxinner pair completes. Multiple snapshots per joinrel are
	 * expected when frontier evolves.
	 */
	foreach(lc, joinrel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		memset(&ev, 0, sizeof(ev));
		ev.relids = pelog_intern_relids(pathprobe_active_log,
										joinrel->relids);
		ev.relid = InvalidOid;
		ev.pathtype = path->pathtype;
		ev.kind = PEK_SURVIVED;
		ev.origin = PATH_ORIGIN_REAL;
		ev.source = PATH_SOURCE_JOIN_HOOK;
		ev.is_parameterized = (path->param_info != NULL);
		ev.cost_startup = path->startup_cost;
		ev.cost_total = path->total_cost;
		ev.disabled_nodes = path->disabled_nodes;

		/* reverse-case annotation (string literals: static lifetime) */
		if (path == cheap_startup && cheap_startup != cheap_total)
			ev.kept_reason = "startup";
		else if (path->param_info == NULL && path != cheap_total &&
				 path->pathkeys != NIL)
			ev.kept_reason = "order?";

		ev.path_id = pelog_path_id(pathprobe_active_log, path);
		pelog_append(pathprobe_active_log, &ev);
	}
}

/* -------------------------------------------------------------------------
 * probe2_join_search_hook
 *
 * join_search_hook: wraps standard_join_search.
 * After each join level completes, records a summary event.
 * In v0.4 we hook at the top level only (not per-level).
 * -------------------------------------------------------------------------
 */
RelOptInfo *
probe2_join_search_hook(PlannerInfo *root, int levels_needed,
						List *initial_rels)
{
	RelOptInfo *result;

	/* call standard or previous hook */
	if (prev_join_search_hook)
		result = prev_join_search_hook(root, levels_needed, initial_rels);
	else
		result = standard_join_search(root, levels_needed, initial_rels);

	return result;
}

/* -------------------------------------------------------------------------
 * probe2_init / probe2_fini - called from pathprobe _PG_init/_PG_fini
 * -------------------------------------------------------------------------
 */
void
probe2_init(void)
{
	prev_join_path_setup_hook = join_path_setup_hook;
	prev_set_join_pathlist_hook = set_join_pathlist_hook;
	prev_join_search_hook = join_search_hook;

	join_path_setup_hook = probe2_join_setup_hook;
	set_join_pathlist_hook = probe2_join_pathlist_hook;
	join_search_hook = probe2_join_search_hook;
}

void
probe2_fini(void)
{
	join_path_setup_hook = prev_join_path_setup_hook;
	set_join_pathlist_hook = prev_set_join_pathlist_hook;
	join_search_hook = prev_join_search_hook;
}
