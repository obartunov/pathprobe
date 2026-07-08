#ifndef PATHPROBE_GATE_H
#define PATHPROBE_GATE_H

#include "postgres.h"
#include "nodes/parsenodes.h"

/*
 * Query gate (scaffolding).
 *
 * Boundary for a future preflight query-load/risk check.  This increment only
 * defines the signal shape and extracts signals already present on an analyzed
 * Query.  There is deliberately no policy here: no thresholds, no allow/reject
 * decision, no workload history, and nothing is wired into the probe path.
 */
typedef struct PathProbeGateSignals
{
	CmdType		command;		/* Query->commandType */
	int			rtable_len;		/* range-table entries */
	bool		has_aggs;		/* Query->hasAggs */
	bool		has_group;		/* grouped */
	bool		has_sort;		/* explicit sort */
	bool		has_distinct;	/* DISTINCT */
	bool		has_sublinks;	/* Query->hasSubLinks */
	bool		has_join;		/* more than one base relation */
}			PathProbeGateSignals;

/*
 * Fill *out with the load signals already available on an analyzed query.
 * Read-only; no estimation and no side effects.  Not yet wired into any
 * decision path.
 */
extern void pathprobe_gate_collect(Query *query, PathProbeGateSignals *out);

#endif							/* PATHPROBE_GATE_H */
