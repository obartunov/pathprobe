#ifndef PATHPROBE_PROPOSE_H
#define PATHPROBE_PROPOSE_H

/*
 * Shared internal interface between the propose mechanism, the advisor
 * (pathprobe_advisor.c), and the query gate (pathprobe_gate.c).  Not a
 * public API and carries no SQL-visible surface.
 */

#include "postgres.h"
#include "access/attnum.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "pathprobe_security.h"

/* ----------
 * ProposedPathSpec - parsed from JSON spec
 * ----------
 */
#define PATHPROBE_MAX_HYPO_COLS 8

typedef struct ProposedPathSpec
{
	/* target */
	char	   *relname;

	/* candidate identity */
	char	   *pathname;
	char	   *provider;

	/* cost */
	Cost		startup_cost;
	Cost		total_cost;
	double		rows;
	int			width;

	/* planner-visible properties */
	bool		parameterized;
	bool		parallel_safe;
	bool		parallel_aware;
	int			npathkeys;
	char	   *pathkey_cols[PATHPROBE_MAX_HYPO_COLS];
	int			disabled_nodes; /* base; winner_forbidden adds 1 more */

	/* mode */
	bool		winner_forbidden;	/* add disabled_nodes=1, prevent winning */

	/* runtime */
	bool		injected;
	bool		target_in_query;	/* relname appears in some range table */
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
	char	   *sql;
	Oid			relid;

	/* runtime */
	bool		injected;
	bool		in_plan;		/* fake oid found in the final plan */
	IndexOptInfo *ioi;			/* identity for event labeling */
	Relids		rel_relids;		/* planner relids of the rel it was injected
								 * into (range-table indexes, not an OID) */
	bool		target_in_query;	/* its relation appears in a range table */
	bool		pred_not_implied;	/* partial; restrictions do not imply it */

	/* build_index_paths() decisions seen for ioi (index path hook) */
	int			build_plain;	/* calls from get_index_paths() */
	int			build_saop;		/* non-native SAOP bitmap retry */
	int			build_or_arm;	/* calls from build_paths_for_OR() */
	int			build_generated;	/* calls that returned paths */
	uint32		build_notgen;	/* HYPO_NOTGEN_* seen in empty calls */
}			HypoIndexSpec;

#define HYPO_NOTGEN_NO_USEFUL_INPUT		0x01
#define HYPO_NOTGEN_NO_LEADING_CLAUSE	0x02
#define HYPO_NOTGEN_SCANTYPE			0x04


/*
 * Advisor atoms kept per relation.  Atoms beyond the limit are not used
 * for candidates; they are counted (advise_atoms_dropped) and reported.
 */
#define PATHPROBE_ADVISE_MAX 6

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
	bool		advise_truncated;	/* candidate cap hit */
	int			advise_atoms_kept;	/* atoms used, summed over rels */
	int			advise_atoms_dropped;	/* distinct atoms over the limit */
	PathProbeAccess *access;	/* ACL decision for this probe (never NULL) */
	PlannerInfo *root;			/* for pathkeys lookup */
	bool		injected;		/* did we inject at least one? */
	bool		winner;			/* did it become cheapest_total? */
}			ProposedPathState;

/*
 * Hypothetical-index builder owned by the propose layer; the advisor calls
 * it to turn a generated expression index into a spec.
 */
extern HypoIndexSpec *hypo_spec_from_sql(char *sql, int idx);

/* local single-query candidate generation (pathprobe_advisor.c) */
extern void pathprobe_advise_generate(PlannerInfo *root, RelOptInfo *rel,
									  RangeTblEntry *rte, ProposedPathState *state);

#endif							/* PATHPROBE_PROPOSE_H */
