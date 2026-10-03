/*-------------------------------------------------------------------------
 * pathprobe.c
 * Part of pathprobe.  Distributed under the PostgreSQL License;
 * see the repository LICENSE file.
 *-------------------------------------------------------------------------
 */
/*
 * pathprobe.c - path-level planner observability for PostgreSQL master
 *
 * Source adapters implemented here:
 *   probe0: set_rel_pathlist_hook  - survivors after add_path() (no patch)
 *
 * probe1 (add_path hook, requires core patch) is in pathprobe_probe1.c
 *
 * Architecture:
 *   hooks -> PathEventLog (append-only, chronological)
 *               ?
 *           PathProbeReport
 *               ?
 *           render_text() / render_json()
 *
 * SQL interface:
 *   pathprobe(query text) RETURNS text
 */

#include "postgres.h"

#include <limits.h>

#include "commands/explain.h"
#include "commands/explain_state.h"
#include "commands/explain_format.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/analyze.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "pathprobe.h"

PG_MODULE_MAGIC;

/* ----------
 * GUC variables
 * Defined here (pathprobe.c), referenced as extern in pathprobe_probe1.c
 * ----------
 */
bool		pathprobe_log_accepted = false; /* log PEK_ACCEPTED events (high
											 * vol) */
double		pathprobe_min_gap = 0.0;	/* skip REJECTED if |gap| < N        */

__attribute__((visibility("default")))
bool		pathprobe_agent_mode = false;
int			pathprobe_max_events = 10000;
int			pathprobe_max_summary_entries = PATHPROBE_MAX_SUMMARY_ENTRIES;
int			pathprobe_max_advisor_candidates = 16;
int			pathprobe_max_hypo_indexes = 64;
int			pathprobe_max_query_length = 100000;
bool		pathprobe_enable_probe2 = true;
static PathProbePathLabelProvider pathprobe_label_provider = NULL;

void
pathprobe_set_path_label_provider(PathProbePathLabelProvider provider)
{
	pathprobe_label_provider = provider;
}

const char *
pathprobe_path_label(const Path *path)
{
	if (pathprobe_label_provider == NULL)
		return NULL;
	return pathprobe_label_provider(path);
}		/* enable join-level hooks
												 * (probe2)  */

/* ----------
 * Global hook state
 * NULL when pathprobe is not active -> hooks are zero-cost passthrough
 * ----------
 */
PathEventLog *pathprobe_active_log = NULL;
PathProbeOpts *pathprobe_active_opts = NULL;

/* Saved hooks */
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;
static ExplainOneQuery_hook_type prev_ExplainOneQuery = NULL;
static bool pathprobe_explain = false;	/* GUC: append PathProbe section to
										 * EXPLAIN */

/* -------------------------------------------------------------------------
 * Utility functions
 * -------------------------------------------------------------------------
 */

/*
 * pathtype_name - NodeTag -> human-readable scan/join name
 *
 * Uses path->pathtype (not path->type).
 * master: pathtype field set by create_*_path() functions.
 */
const char *
pathtype_name(NodeTag pt)
{
	switch (pt)
	{
			/* base scans */
		case T_SeqScan:
			return "SeqScan";
		case T_SampleScan:
			return "SampleScan";
		case T_IndexScan:
			return "IndexScan";
		case T_IndexOnlyScan:
			return "IndexOnlyScan";
		case T_BitmapHeapScan:
			return "BitmapHeapScan";
		case T_BitmapAnd:
			return "BitmapAnd";
		case T_BitmapOr:
			return "BitmapOr";
		case T_TidScan:
			return "TidScan";
		case T_TidRangeScan:
			return "TidRangeScan";
		case T_SubqueryScan:
			return "SubqueryScan";
		case T_FunctionScan:
			return "FunctionScan";
		case T_ValuesScan:
			return "ValuesScan";
		case T_CteScan:
			return "CteScan";
		case T_WorkTableScan:
			return "WorkTableScan";
		case T_ForeignScan:
			return "ForeignScan";
		case T_CustomScan:
			return "CustomScan";
			/* append */
		case T_Append:
			return "Append";
		case T_MergeAppend:
			return "MergeAppend";
			/* upper-rel / plan shaping - critical for ORDER BY, LIMIT, agg */
		case T_Result:
			return "Result";
		case T_Material:
			return "Material";
		case T_Memoize:
			return "Memoize";
		case T_Sort:
			return "Sort";
		case T_IncrementalSort:
			return "IncrementalSort";
		case T_Group:
			return "Group";
		case T_Agg:
			return "Agg";
		case T_WindowAgg:
			return "WindowAgg";
		case T_Unique:
			return "Unique";
		case T_SetOp:
			return "SetOp";
		case T_LockRows:
			return "LockRows";
		case T_Limit:
			return "Limit";
		case T_ModifyTable:
			return "ModifyTable";
			/* joins */
		case T_NestLoop:
			return "NestLoop";
		case T_MergeJoin:
			return "MergeJoin";
		case T_HashJoin:
			return "HashJoin";
			/* parallel */
		case T_Gather:
			return "Gather";
		case T_GatherMerge:
			return "GatherMerge";
		default:
			{
				/* emit numeric tag so it is never silently lost */
				static char tagbuf[32];

				snprintf(tagbuf, sizeof(tagbuf), "NodeTag(%d)", (int) pt);
				return tagbuf;
			}
	}
}

/* Relids bitmapset -> "1.3.5" string */
void
relids_to_str(Relids r, char *buf, int len)
{
	int			x = -1;
	int			pos = 0;
	bool		first = true;

	if (len < 4)
	{
		buf[0] = '\0';
		return;
	}
	buf[pos++] = '{';
	while ((x = bms_next_member(r, x)) >= 0 && pos < len - 4)
	{
		if (!first)
			buf[pos++] = ',';
		pos += snprintf(buf + pos, len - pos, "%d", x);
		first = false;
	}
	buf[pos++] = '}';
	buf[pos] = '\0';
}

/* FNV-1a over debug_query_string */
uint64
pathprobe_qfp(void)
{
	const char *s = debug_query_string;
	uint64		h = UINT64CONST(14695981039346656037);
	unsigned char c;

	if (!s)
		return 0;
	while ((c = (unsigned char) *s++) != 0)
	{
		h ^= c;
		h *= UINT64CONST(1099511628211);
	}
	return h;
}

/* -------------------------------------------------------------------------
 * PathEventLog - primary layer
 * -------------------------------------------------------------------------
 */

uint32
pathprobe_coverage(void)
{
	uint32		cov = PP_COV_SURVIVED;

#ifdef HAVE_PATHPROBE_HOOKS
	cov |= PP_COV_PRECHECK;
	/* with min_gap > 0 the rejected hook drops events whose gap is 0 */
	if (pathprobe_min_gap <= 0.0)
		cov |= PP_COV_REJECTED;
	if (pathprobe_log_accepted)
		cov |= PP_COV_ACCEPTED | PP_COV_DISPLACED;
#endif
	if (pathprobe_enable_probe2)
		cov |= PP_COV_EXCLUDED;
	return cov;
}

PathEventLog *
pelog_create(MemoryContext mcxt)
{
	PathEventLog *log;
	MemoryContext old = MemoryContextSwitchTo(mcxt);

	log = palloc0(sizeof(PathEventLog));
	log->capacity = 128;
	log->events = palloc(log->capacity * sizeof(PathEvent));
	log->nevents = 0;
	log->mcxt = mcxt;
	log->coverage = pathprobe_coverage();

	MemoryContextSwitchTo(old);
	return log;
}


/*
 * pathprobe_json_str - escape and emit a JSON string (full: quotes,
 * backslash, control characters as \u00XX).  NULL emits JSON null.
 * Single sink for every user-supplied or derived string field.
 */
void
pathprobe_json_str(StringInfo out, const char *s)
{
	if (s == NULL)
	{
		appendStringInfoString(out, "null");
		return;
	}
	appendStringInfoChar(out, '"');
	for (; *s; s++)
	{
		switch (*s)
		{
			case '"':
				appendStringInfoString(out, "\\\"");
				break;
			case '\\':
				appendStringInfoString(out, "\\\\");
				break;
			case '\n':
				appendStringInfoString(out, "\\n");
				break;
			case '\r':
				appendStringInfoString(out, "\\r");
				break;
			case '\t':
				appendStringInfoString(out, "\\t");
				break;
			case '\b':
				appendStringInfoString(out, "\\b");
				break;
			case '\f':
				appendStringInfoString(out, "\\f");
				break;
			default:
				if ((unsigned char) *s < 0x20)
					appendStringInfo(out, "\\u%04x", (unsigned char) *s);
				else
					appendStringInfoChar(out, *s);
				break;
		}
	}
	appendStringInfoChar(out, '"');
}

/*
 * pelog_append - single point of insertion, assigns seq
 *
 * Copies *ev by value. Caller owns the source (stack or palloc).
 * seq is assigned here; never by caller.
 */
void
pelog_append(PathEventLog *log, PathEvent *ev)
{
	MemoryContext old;

	{
		/* agent-safe hard cap: never grow silently past the limit -
		 * the truncation is reported, not swallowed.  agent_mode clamps
		 * the effective cap locally; GUC variables are never mutated. */
		int			cap = pathprobe_agent_mode ?
			Min(pathprobe_max_events, 10000) : pathprobe_max_events;

		if (log->nevents >= cap)
		{
			log->events_truncated = true;
			return;
		}
	}
	if (log->nevents >= log->capacity)
	{
		old = MemoryContextSwitchTo(log->mcxt);
		log->capacity *= 2;
		log->events = repalloc(log->events,
							   log->capacity * sizeof(PathEvent));
		MemoryContextSwitchTo(old);
	}

	ev->seq = log->nevents + 1; /* monotone, 1-based */
	log->events[log->nevents++] = *ev;	/* copy by value     */
}

/* -------------------------------------------------------------------------
 * pelog_build_rtmap
 *
 * Build rtindex -> relname mapping from Query->rtable.
 * Called after pg_analyze_and_rewrite, before pg_plan_queries.
 * probe1 hook uses this to resolve "{N}" -> "orders".
 * -------------------------------------------------------------------------
 */
void
pelog_build_rtmap(PathEventLog *log, List *rtable)
{
	MemoryContext old;
	int			nentries = list_length(rtable);
	PathRTMap  *rtmap;
	int			i;
	ListCell   *lc;

	if (nentries == 0)
		return;

	old = MemoryContextSwitchTo(log->mcxt);
	rtmap = palloc0(sizeof(PathRTMap));
	rtmap->nentries = nentries;
	rtmap->names = palloc0(nentries * sizeof(char *));

	i = 0;
	foreach(lc, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		/*
		 * Only store RTE_RELATION entries - actual tables and views. Skip
		 * joins, subqueries, functions: they have no meaningful relation name
		 * and never appear in probe1 baserel relids. Use eref->aliasname
		 * (same as EXPLAIN: reflects user aliases).
		 */
		if (rte->rtekind == RTE_RELATION &&
			rte->eref && rte->eref->aliasname && rte->eref->aliasname[0])
			rtmap->names[i] = pstrdup(rte->eref->aliasname);
		else
			rtmap->names[i] = NULL;
		i++;
	}

	log->rtmap = rtmap;
	MemoryContextSwitchTo(old);
}

/* -------------------------------------------------------------------------
 * pelog_intern_relids
 *
 * Return a canonical pointer for relids r, shared across events.
 * First occurrence: bms_copy into log->mcxt, cached in pool.
 * Subsequent equal bitmaps: return cached pointer (no allocation).
 *
 * Avoids O(n_events) bms_copy calls - only O(unique_relids) copies.
 * Falls back to direct bms_copy if pool is full (rare: >32 distinct rels).
 * -------------------------------------------------------------------------
 */
Relids
pelog_intern_relids(PathEventLog *log, Relids r)
{
	PathRelidsPool *pool = &log->relids_pool;
	MemoryContext old;
	Relids		copy;
	int			i;

	if (r == NULL || bms_is_empty(r))
		return NULL;

	/* Search pool for existing equal bitmap */
	for (i = 0; i < pool->nkeys; i++)
		if (bms_equal(pool->keys[i], r))
			return pool->keys[i];

	/* Not found: copy into log mcxt and cache */
	old = MemoryContextSwitchTo(log->mcxt);
	copy = bms_copy(r);
	MemoryContextSwitchTo(old);

	if (pool->nkeys < PATHPROBE_RELIDS_POOL_SIZE)
		pool->keys[pool->nkeys++] = copy;
	/* if pool full: copy is still valid, just not cached for future reuse */

	return copy;
}

/* -------------------------------------------------------------------------
 * probe0: set_rel_pathlist_hook
 *
 * Records survivors from rel->pathlist after add_path() has finished.
 * kind=PEK_SURVIVED, source=PATH_SOURCE_REL_HOOK.
 *
 * Limitation: does not see rejected paths - that requires probe1 (core patch).
 * -------------------------------------------------------------------------
 */
static void
probe0_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
						Index rti, RangeTblEntry *rte)
{
	ListCell   *lc;
	PathEvent	ev;
	char	   *relname;
	Path	   *cheap_total = NULL;
	Path	   *cheap_startup = NULL;

	/* Passthrough if not active */
	if (pathprobe_active_log == NULL)
		goto chain;

	/* v0: base relations only; join rels handled by join hook later */
	if (rel->reloptkind != RELOPT_BASEREL)
		goto chain;

	/*
	 * If probe1 (add_path hook) is active, it already recorded events for
	 * every path at competition time. probe0 would duplicate those as
	 * PEK_SURVIVED with source=REL_HOOK.
	 *
	 * We still emit probe0 events so the log shows the final frontier
	 * explicitly - useful when probe1 is absent. When probe1 is present,
	 * source field distinguishes the two layers.
	 *
	 * Future: add opt-in dedup by (relid, pathtype) if log grows noisy.
	 */

	relname = OidIsValid(rte->relid) ? get_rel_name(rte->relid) : "?";

	/*
	 * Reverse case: a survivor that is NOT the rel's cheapest_total is kept
	 * because it is non-dominated on another dimension a parent may need.
	 * set_cheapest() has not run yet at this hook, so derive cheapest_total
	 * and cheapest_startup from the frontier ourselves (unparameterized
	 * only).
	 */
	{
		ListCell   *lc2;

		foreach(lc2, rel->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc2);

			if (p->param_info != NULL)
				continue;
			if (cheap_total == NULL || p->total_cost < cheap_total->total_cost)
				cheap_total = p;
			if (cheap_startup == NULL || p->startup_cost < cheap_startup->startup_cost)
				cheap_startup = p;
		}
	}

	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		memset(&ev, 0, sizeof(ev));
		ev.relid = rte->relid;
		ev.pathtype = path->pathtype;
		ev.kind = PEK_SURVIVED;
		ev.origin = PATH_ORIGIN_REAL;
		ev.source = PATH_SOURCE_REL_HOOK;
		ev.is_parameterized = (path->param_info != NULL);
		ev.cost_startup = path->startup_cost;
		ev.cost_total = path->total_cost;
		ev.disabled_nodes = path->disabled_nodes;
		ev.relids = bms_copy(rel->relids);	/* snapshot */
		strlcpy(ev.relname, relname, sizeof(ev.relname));

		/* reverse-case annotation (string literals: static lifetime) */
		if (path == cheap_startup && cheap_startup != cheap_total)
			ev.label = "  kept: startup";
		else if (path->param_info == NULL && path != cheap_total &&
				 path->pathkeys != NIL)
			ev.label = "  kept: order?";

		pelog_append(pathprobe_active_log, &ev);
	}

chain:
	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);
}

/* -------------------------------------------------------------------------
 * pathprobe_internal
 *
 * Parse -> analyze -> plan query_string, collecting events via active hooks.
 * Returns PathProbeReport in CurrentMemoryContext child.
 * No executor invocation.
 * -------------------------------------------------------------------------
 */
PathProbeReport *
pathprobe_internal(const char *query_string, PathProbeOpts *opts)
{
	MemoryContext log_mcxt;
	PathEventLog *log;
	PathProbeReport *report;
	List	   *parsetree_list;
	ListCell   *lc;

	log_mcxt = AllocSetContextCreate(CurrentMemoryContext,
									 "PathProbeLog",
									 ALLOCSET_DEFAULT_SIZES);
	log = pelog_create(log_mcxt);

	/* Activate hooks */
	pathprobe_active_log = log;
	pathprobe_active_opts = opts;

	PG_TRY();
	{
		parsetree_list = pg_parse_query(query_string);

		foreach(lc, parsetree_list)
		{
			RawStmt    *parsetree = lfirst_node(RawStmt, lc);
			List	   *querytree_list;
			List	   *plantree_list;

			querytree_list = pg_analyze_and_rewrite_fixedparams(
																parsetree,
																query_string,
																NULL, 0,
																NULL);

			/*
			 * Build rtindex->relname map from analyzed query before planning.
			 * probe1 hook uses this to show real names instead of "{N}".
			 */
			if (querytree_list != NIL)
			{
				Query	   *q = linitial_node(Query, querytree_list);

				if (q->rtable)
					pelog_build_rtmap(log, q->rtable);
			}

			/* Hooks fire during planning */
			plantree_list = pg_plan_queries(querytree_list,
											query_string,
											CURSOR_OPT_PARALLEL_OK,
											NULL);
			(void) plantree_list;
		}
	}
	PG_CATCH();
	{
		pathprobe_active_log = NULL;
		pathprobe_active_opts = NULL;
		PG_RE_THROW();
	}
	PG_END_TRY();

	pathprobe_active_log = NULL;
	pathprobe_active_opts = NULL;

	report = MemoryContextAlloc(log_mcxt, sizeof(PathProbeReport));
	report->log = log;

	return report;
}

/* -------------------------------------------------------------------------
 * pelog_build_summary
 *
 * Build per-(relids, pathtype) aggregation over the event log.
 * Groups events by relation and path strategy, counts per kind.
 * Used by render_text summary block.
 * -------------------------------------------------------------------------
 */
PathSummary *
pelog_build_summary(PathEventLog *log, MemoryContext mcxt)
{
	PathSummary *sum;
	MemoryContext old;
	int			i;

	old = MemoryContextSwitchTo(mcxt);
	sum = palloc0(sizeof(PathSummary));
	MemoryContextSwitchTo(old);

	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];
		PathSummaryEntry *ent = NULL;
		int			j;

		/* Skip upper-rel paths with no relids */
		if (ev->relids == NULL || bms_is_empty(ev->relids))
			continue;

		/* Find existing entry for (relids, pathtype) */
		for (j = 0; j < sum->nentries; j++)
		{
			if (sum->entries[j].pathtype == ev->pathtype &&
				bms_equal(sum->entries[j].relids, ev->relids))
			{
				ent = &sum->entries[j];
				break;
			}
		}

		/* Create new entry if not found */
		if (ent == NULL)
		{
			if (sum->nentries >= pathprobe_max_summary_entries ||
				sum->nentries >= PATHPROBE_MAX_SUMMARY_ENTRIES)
			{
				sum->truncated = true;	/* reported, not swallowed */
				continue;
			}
			ent = &sum->entries[sum->nentries++];
			old = MemoryContextSwitchTo(mcxt);
			ent->relids = bms_copy(ev->relids);
			MemoryContextSwitchTo(old);
			ent->pathtype = ev->pathtype;
		}

		/* Accumulate counts */
		switch (ev->kind)
		{
			case PEK_EXCLUDED:
				ent->n_excluded++;
				break;
			case PEK_SKIPPED:
				ent->n_skipped++;
				break;
			case PEK_REJECTED:
				ent->n_rejected++;
				break;
			case PEK_ACCEPTED:
				ent->n_accepted++;
				break;
			case PEK_DISPLACED:
				break;
			case PEK_SURVIVED:
				ent->n_survived++;
				if (!ent->has_winner || ev->cost_total < ent->best_cost)
				{
					ent->best_cost = ev->cost_total;
					ent->has_winner = true;
				}
				break;
		}
	}

	return sum;
}

/* -------------------------------------------------------------------------
 * render_text - chronological dump, read-only, idempotent
 * -------------------------------------------------------------------------
 */
void
render_text(PathProbeReport *report, StringInfo out, PathProbeOpts *opts)
{
	PathEventLog *log = report->log;
	int			i;

	bool		has_probe1 = (log->coverage & PP_COV_PRECHECK) != 0;
	int			i2;

	if (has_probe1)
		appendStringInfoString(out,
							   "PathProbe  [probe0+probe1+precheck: full path life visible]\n");
	else
		appendStringInfoString(out,
							   "PathProbe  [probe0 only: survivors; REJECTED requires probe1 patch]\n");

	/* Emit rel id->name legend from rtmap */
	if (log->rtmap && log->rtmap->nentries > 0)
	{
		appendStringInfoString(out, "rels: ");
		for (i2 = 0; i2 < log->rtmap->nentries; i2++)
		{
			if (log->rtmap->names[i2])
				appendStringInfo(out, "%d=%s  ",
								 i2 + 1,
								 log->rtmap->names[i2]);
		}
		appendStringInfoChar(out, '\n');
	}

	appendStringInfoString(out,
						   "-------------------------------------------------------------------\n"
						   " seq  src  rel           pathtype              cost_total  gap                     event\n"
						   "-------------------------------------------------------------------\n");

	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];
		const char *kind_str;
		const char *origin_str;

		switch (ev->kind)
		{
			case PEK_EXCLUDED:
				kind_str = "EXCLUDED";
				break;			/* pgs_mask: never generated */
			case PEK_SKIPPED:
				kind_str = "SKIPPED";
				break;			/* precheck: never built */
			case PEK_REJECTED:
				kind_str = "REJECTED";
				break;			/* add_path: lost competition */
			case PEK_ACCEPTED:
				kind_str = "accepted";
				break;
			case PEK_DISPLACED:
				kind_str = "displaced";
				break;
			case PEK_SURVIVED:
				kind_str = "survived";
				break;
			default:
				kind_str = "?";
				break;
		}

		origin_str = (ev->origin == PATH_ORIGIN_PROPOSED) ? " [proposed]" : "";

		{
			const char *src_str;
			char		rel_buf[64];
			char		gap_buf[24];
			char		cost_col[16];

			/* source label */
			switch (ev->source)
			{
				case PATH_SOURCE_PRECHECK:
					src_str = "pc";
					break;
				case PATH_SOURCE_ADD_PATH:
					src_str = "p1";
					break;
				case PATH_SOURCE_REL_HOOK:
					src_str = "p0";
					break;
				case PATH_SOURCE_JOIN_HOOK:
					src_str = "j2";
					break;
				case PATH_SOURCE_PROPOSED:
					src_str = "wp";
					break;
				default:
					src_str = "?";
					break;
			}

			/*
			 * relation label: always use numeric relids for scalability.
			 * Legend printed once in header via rtmap. Upper-rel paths with
			 * no relids show "?".
			 */
			if (ev->relids && !bms_is_empty(ev->relids))
				relids_to_str(ev->relids, rel_buf, sizeof(rel_buf));
			else
				strlcpy(rel_buf, "?", sizeof(rel_buf));

			/* gap label - semantics depend on kind */
			if (ev->kind == PEK_SKIPPED)
			{
				/*
				 * show skip reason; cost is lower bound only, no meaningful
				 * gap
				 */
				const char *reason =
					(ev->skip_reason == SKIP_DOMINATED_EARLY) ? "dominated_early" :
					(ev->skip_reason == SKIP_DISABLED) ? "disabled" :
					(ev->skip_reason == SKIP_PARAM_MISMATCH) ? "param_mismatch" :
					"unknown";

				strlcpy(gap_buf, reason, sizeof(gap_buf));
			}
			else if (ev->gap_cost != 0.0)
			{
				const char *gap_label =
					(ev->kind == PEK_REJECTED) ? "lost_by" : "impr_by";

				if (opts->hide_costs)
					strlcpy(gap_buf, gap_label, sizeof(gap_buf));
				else
					snprintf(gap_buf, sizeof(gap_buf), "%s=%+.2f",
							 gap_label, ev->gap_cost);
			}
			else
				strlcpy(gap_buf, "", sizeof(gap_buf));

			if (opts->hide_costs)
				strlcpy(cost_col, "          ", sizeof(cost_col));
			else
				snprintf(cost_col, sizeof(cost_col), "%10.2f", ev->cost_total);

			appendStringInfo(out,
							 "#%-4d %s  %-12s  %-20s  %s  %-22s  %s%s%s%s%s%s\n",
							 ev->seq,
							 src_str,
							 rel_buf,
							 pathtype_name(ev->pathtype),
							 cost_col,
							 gap_buf,
							 kind_str,
							 (ev->is_parameterized && !OidIsValid(ev->relid))
							 ? " [outer]"
							 : (ev->is_parameterized ? " [param]" : ""),
							 origin_str,
							 (ev->disabled_nodes > 0) ? " disabled" : "",
							 (ev->label && ev->label[0] != ' ') ? " " : "",
							 ev->label ? ev->label : "");

			/* verbose: competitor name */
			if (opts->verbose && ev->other_pathtype != T_Invalid)
				appendStringInfo(out,
								 "             vs=%s\n",
								 pathtype_name(ev->other_pathtype));
		}
	}

	appendStringInfoString(out,
						   "-------------------------------------------------------------------\n");
	appendStringInfo(out, "total events: %d\n", log->nevents);

	/* -----------------------------------------------------------------------
     * Summary block - aggregation per (relids, pathtype)
     * Shows elimination profile: how many candidates at each stage.
     * ----------------------------------------------------------------------- */
	{
		PathSummary *sum;
		Relids		last_relids = NULL;
		int			j;

		sum = pelog_build_summary(log, CurrentMemoryContext);
		if (sum->nentries > 0)
		{
			appendStringInfoString(out,
								   "\n"
								   "Summary (per relation x strategy)\n"
								   "-------------------------------------------------------------------\n"
								   "  rel           strategy             excl  skip  rej  acc  surv  best_cost\n"
								   "-------------------------------------------------------------------\n");

			for (j = 0; j < sum->nentries; j++)
			{
				PathSummaryEntry *ent = &sum->entries[j];
				char		rel_buf[32];
				char		cost_buf[16];

				relids_to_str(ent->relids, rel_buf, sizeof(rel_buf));

				/* blank separator between different rels */
				if (last_relids && !bms_equal(last_relids, ent->relids))
					appendStringInfoChar(out, '\n');
				last_relids = ent->relids;

				if (opts->hide_costs)
					strlcpy(cost_buf, "         -", sizeof(cost_buf));
				else if (ent->has_winner)
					snprintf(cost_buf, sizeof(cost_buf), "%10.2f", ent->best_cost);
				else
					strlcpy(cost_buf, "         -", sizeof(cost_buf));

				appendStringInfo(out,
								 "  %-12s  %-20s %4d  %4d  %3d  %3d  %4d  %s\n",
								 rel_buf,
								 pathtype_name(ent->pathtype),
								 ent->n_excluded,
								 ent->n_skipped,
								 ent->n_rejected,
								 ent->n_accepted,
								 ent->n_survived,
								 cost_buf);
			}
			appendStringInfoString(out,
								   "-------------------------------------------------------------------\n");
		}
		pfree(sum);
	}
}

/* -------------------------------------------------------------------------
 * _PG_init / _PG_fini
 * -------------------------------------------------------------------------
 */
void		_PG_init(void);
void		_PG_fini(void);

static void pathprobe_emit_structured(PathEventLog *log, ExplainState *es);

/* active only between ExplainOneQuery entry and exit (single backend) */
static PathEventLog *pathprobe_explain_log = NULL;
static PathProbeOpts pathprobe_explain_opts;
static explain_per_plan_hook_type prev_explain_per_plan = NULL;

/*
 * pathprobe_per_plan
 *
 * Fires inside ExplainOnePlan, i.e. inside the per-query output group, which
 * is the only place structured (JSON/XML/YAML) properties may be added.
 * The event log has been filled during planning by probe0/probe1/probe2.
 */
static void
pathprobe_per_plan(PlannedStmt *plannedstmt, IntoClause *into,
				   ExplainState *es, const char *queryString,
				   ParamListInfo params, QueryEnvironment *queryEnv)
{
	if (prev_explain_per_plan)
		prev_explain_per_plan(plannedstmt, into, es, queryString,
							  params, queryEnv);

	if (pathprobe_explain_log == NULL)
		return;

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		PathProbeReport report;

		report.log = pathprobe_explain_log;
		report.injected = false;
		appendStringInfoChar(es->str, '\n');
		render_text(&report, es->str, &pathprobe_explain_opts);
	}
	else
		pathprobe_emit_structured(pathprobe_explain_log, es);
}

/*
 * pathprobe_ExplainOneQuery
 *
 * Inline front-end: when pathprobe.explain is on, activate the event log for
 * the duration of the real EXPLAIN planning so probe0/probe2 (and probe1 when
 * the core patch is present) fill it, then append the PathProbe section to the
 * EXPLAIN text output.  References only stock pathprobe state - no patched
 * symbols - so this front-end works on stock PostgreSQL.
 */
static void
pathprobe_ExplainOneQuery(Query *query, int cursorOptions, IntoClause *into,
						  ExplainState *es, const char *queryString,
						  ParamListInfo params, QueryEnvironment *queryEnv)
{
	MemoryContext log_mcxt = NULL;
	PathEventLog *log = NULL;
	PathProbeOpts opts = {false, false, false};
	bool		active = pathprobe_explain && pathprobe_active_log == NULL;

	if (active)
	{
		opts.hide_costs = !es->costs;
		log_mcxt = AllocSetContextCreate(CurrentMemoryContext,
										 "PathProbeExplain",
										 ALLOCSET_DEFAULT_SIZES);
		log = pelog_create(log_mcxt);
		if (query->rtable)
			pelog_build_rtmap(log, query->rtable);
		pathprobe_active_log = log;
		pathprobe_active_opts = &opts;
		pathprobe_explain_log = log;	/* consumed by pathprobe_per_plan */
		pathprobe_explain_opts = opts;
	}

	PG_TRY();
	{
		if (prev_ExplainOneQuery)
			prev_ExplainOneQuery(query, cursorOptions, into, es, queryString,
								 params, queryEnv);
		else
			standard_ExplainOneQuery(query, cursorOptions, into, es, queryString,
									 params, queryEnv);
	}
	PG_FINALLY();
	{
		if (active)
		{
			pathprobe_active_log = NULL;
			pathprobe_active_opts = NULL;
			pathprobe_explain_log = NULL;
		}
	}
	PG_END_TRY();

	if (active)
		MemoryContextDelete(log_mcxt);
}

/*
 * pathprobe_emit_structured
 *
 * Emit the event log into EXPLAIN's structured output (JSON/XML/YAML) via
 * the ExplainProperty API.  Raw text must not be appended to es->str for
 * these formats, or the output would be invalid.  Cost fields are omitted
 * under COSTS OFF so agent-facing fields stay stable.
 */
static void
pathprobe_emit_structured(PathEventLog *log, ExplainState *es)
{
	int			i;

	ExplainOpenGroup("PathProbe", "PathProbe", false, es);
	for (i = 0; i < log->nevents; i++)
	{
		PathEvent  *ev = &log->events[i];
		const char *kind_str;
		char		rel_buf[64];

		switch (ev->kind)
		{
			case PEK_EXCLUDED:
				kind_str = "excluded";
				break;
			case PEK_SKIPPED:
				kind_str = "skipped";
				break;
			case PEK_REJECTED:
				kind_str = "rejected";
				break;
			case PEK_ACCEPTED:
				kind_str = "accepted";
				break;
			case PEK_DISPLACED:
				kind_str = "displaced";
				break;
			case PEK_SURVIVED:
			default:
				kind_str = "survived";
				break;
		}

		if (ev->relids && !bms_is_empty(ev->relids))
			relids_to_str(ev->relids, rel_buf, sizeof(rel_buf));
		else
			strlcpy(rel_buf, "?", sizeof(rel_buf));

		ExplainOpenGroup("Path", NULL, true, es);
		ExplainPropertyInteger("Seq", NULL, ev->seq, es);
		ExplainPropertyText("Rel", rel_buf, es);
		ExplainPropertyText("Path Type", pathtype_name(ev->pathtype), es);
		ExplainPropertyText("Event", kind_str, es);
		ExplainPropertyBool("Parameterized", ev->is_parameterized, es);
		if (es->costs)
		{
			ExplainPropertyFloat("Startup Cost", NULL, ev->cost_startup, 2, es);
			ExplainPropertyFloat("Total Cost", NULL, ev->cost_total, 2, es);
			if (ev->gap_cost != 0.0)
				ExplainPropertyFloat("Gap", NULL, ev->gap_cost, 2, es);
		}
		if (ev->disabled_nodes > 0)
			ExplainPropertyInteger("Disabled Nodes", NULL, ev->disabled_nodes, es);
		if (ev->label)
		{
			/* strip the "  kept: " text-render padding for the field value */
			const char *lbl = ev->label;

			while (*lbl == ' ')
				lbl++;
			ExplainPropertyText("Kept", lbl, es);
		}
		ExplainCloseGroup("Path", NULL, true, es);
	}
	ExplainCloseGroup("PathProbe", "PathProbe", false, es);
}

void
_PG_init(void)
{
	DefineCustomBoolVariable(
							 "pathprobe.log_accepted",
							 "Log PEK_ACCEPTED events from add_path (high volume; probe1 only)",
							 NULL,
							 &pathprobe_log_accepted,
							 false,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable("pathprobe.agent_mode",
							 "Conservative agent profile: enforced limits, planning-only, machine fields on.",
							 NULL, &pathprobe_agent_mode, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pathprobe.max_events",
							"Hard cap on recorded path events per run.",
							NULL, &pathprobe_max_events, 10000, 16,
							INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pathprobe.max_summary_entries",
							"Cap on summary table entries (<= compile-time table size).",
							NULL, &pathprobe_max_summary_entries,
							PATHPROBE_MAX_SUMMARY_ENTRIES, 1,
							PATHPROBE_MAX_SUMMARY_ENTRIES,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pathprobe.max_advisor_candidates",
							"Cap on advisor-generated hypothetical indexes per query.",
							NULL, &pathprobe_max_advisor_candidates, 16, 1,
							256, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pathprobe.max_hypo_indexes",
							"Cap on user-supplied hypothetical indexes per call.",
							NULL, &pathprobe_max_hypo_indexes, 64, 1,
							1024, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("pathprobe.max_query_length",
							"Reject probe queries longer than this many bytes.",
							NULL, &pathprobe_max_query_length, 100000, 64,
							INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable(
							 "pathprobe.enable_probe2",
							 "Enable probe2 join-level hooks (setup/pathlist). Disable to reduce overhead.",
							 NULL,
							 &pathprobe_enable_probe2,
							 true,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomRealVariable(
							 "pathprobe.min_gap",
							 "Skip REJECTED events with |gap| below this threshold (probe1 only)",
							 NULL,
							 &pathprobe_min_gap,
							 0.0, 0.0, 1e12,
							 PGC_SUSET, 0, NULL, NULL, NULL);

	DefineCustomBoolVariable(
							 "pathprobe.explain",
							 "Append a PathProbe section (all considered paths) to EXPLAIN output",
							 NULL,
							 &pathprobe_explain,
							 false,
							 PGC_USERSET, 0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("pathprobe");
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = probe0_set_rel_pathlist;

	prev_ExplainOneQuery = ExplainOneQuery_hook;
	ExplainOneQuery_hook = pathprobe_ExplainOneQuery;

	prev_explain_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = pathprobe_per_plan;

	/*
	 * probe1: register add_path hooks, but only if the core patch is present.
	 * The patch defines HAVE_PATHPROBE_HOOKS and the hook globals; without it
	 * these references do not exist, so we compile them out and run
	 * probe0+probe2 only (fully stock, no core patch required).
	 */
#ifdef HAVE_PATHPROBE_HOOKS
	pathprobe_path_rejected_hook = probe1_rejected_hook;
	pathprobe_path_accepted_hook = probe1_accepted_hook;
	pathprobe_path_skipped_hook = probe_skipped_hook;
#endif

	/* probe2: join-level hooks (no core patch required) */
	/* hooks always installed; the GUC gates logging inside (runtime-safe) */
	probe2_init();

#ifdef HAVE_PATHPROBE_HOOKS
	elog(LOG, "pathprobe: probe0+probe1+probe2+precheck active");
#else
	elog(LOG, "pathprobe: probe0+probe2 active (stock; probe1 needs pathprobe_add_path patch)");
#endif
}

void
_PG_fini(void)
{
	set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
	ExplainOneQuery_hook = prev_ExplainOneQuery;
	explain_per_plan_hook = prev_explain_per_plan;
#ifdef HAVE_PATHPROBE_HOOKS
	pathprobe_path_rejected_hook = NULL;
	pathprobe_path_accepted_hook = NULL;
	pathprobe_path_skipped_hook = NULL;
#endif
	probe2_fini();				/* safe to call even if not init'd */
}
