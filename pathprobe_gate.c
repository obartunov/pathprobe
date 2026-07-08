/*
 * pathprobe_gate.c
 *		Preflight query-load/risk boundary (scaffolding).
 *
 *		Establishes the layer that will host the agent/query-load gate so that
 *		such logic does not accumulate inside pathprobe_propose.c.  This
 *		increment extracts already-available structural signals from an
 *		analyzed Query and defines their shape.  It carries no policy: no
 *		thresholds, no allow/reject, no statistics, and it is not wired into
 *		the probe path.  Behaviour is unchanged.
 */
#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

#include "pathprobe_gate.h"

void
pathprobe_gate_collect(Query *query, PathProbeGateSignals *out)
{
	int			nbaserel = 0;
	ListCell   *lc;

	memset(out, 0, sizeof(*out));
	if (query == NULL)
		return;

	out->command = query->commandType;
	out->rtable_len = list_length(query->rtable);
	out->has_aggs = query->hasAggs;
	out->has_group = query->groupClause != NIL;
	out->has_sort = query->sortClause != NIL;
	out->has_distinct = query->distinctClause != NIL;
	out->has_sublinks = query->hasSubLinks;

	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_RELATION)
			nbaserel++;
	}
	out->has_join = nbaserel > 1;
}
