# JSON format

The JSON output is the stable surface for tools and agents.

`version` is the output-format version.  It is not necessarily the package
version or the extension SQL version.

## Top-level shape

```json
{
  "version": "1.1",
  "query": "SELECT ...",
  "rels": {"1": "t"},
  "advisor": {},
  "hypo_indexes": [],
  "candidates": [],
  "events": [],
  "paths": [],
  "limits": {},
  "coverage": {},
  "summary": []
}
```

Not every top-level field is meaningful in every mode, but machine readers
should tolerate empty arrays/objects.

## events[]

```json
{
  "seq": 1,
  "src": "wp",
  "rel": "{1}",
  "pathtype": "CustomScan",
  "kind": "accepted",
  "origin": "proposed",
  "label": "candidate_name",
  "path_id": 4,
  "cost_startup": 0.0,
  "cost_total": 1.0,
  "disabled_nodes": 1
}
```

A DISPLACED event also names the path that displaced it:

```json
{
  "seq": 19, "src": "p1", "rel": "{1,2}", "pathtype": "MergeJoin",
  "kind": "displaced", "path_id": 9,
  "displacer_path_id": 11, "displacer_seq": 18, "other_pathtype": "NestLoop",
  "gap_cost": 773.13
}
```

A REJECTED event names the old path that made it lose:

```json
{
  "seq": 20, "src": "p1", "rel": "{1,2}", "pathtype": "NestLoop",
  "kind": "rejected", "path_id": 14,
  "dominator_path_id": 13, "dominator_pathtype": "MergeJoin",
  "dominator_seq": 17
}
```

A REMOVED event names the path that removed it, which `add_path()` then
rejected (see "Removed before a rejection" below):

```json
{
  "seq": 8, "src": "wp", "rel": "{1}", "pathtype": "CustomScan",
  "kind": "removed", "label": "cA", "path_id": 2,
  "remover_path_id": 4, "remover_seq": 7, "remover_pathtype": "CustomScan"
}
```

Rules:

```text
label is set only for proposed/hypothetical/advisor paths
origin is real or proposed; it is proposed if label is set
kept_reason is "startup" or "order?" on SURVIVED events (p0, j2) of a real
  path kept in the pathlist without being the cheapest total; absent
  otherwise
cost fields are omitted when not meaningful or when costs are suppressed
cost_startup and cost_total are the path's costs at the event; for skipped
  events, the lower bounds add_path_precheck() compared
strings are JSON-escaped, never interpolated raw
path_id is null for skipped and excluded events (no Path exists yet)
displacer_seq is null if the displacer's ACCEPTED event was not recorded
  (event cap)
dominator_seq is null if the dominator's ACCEPTED event into this rel was
  not recorded (pathprobe.log_accepted off, event cap, or the path was put
  into the pathlist without add_path())
dominator_path_id and dominator_pathtype are null only with a server built
  from an older copy of the core patch (see below)
remover_seq is null if the remover's REJECTED event was not recorded
  (pathprobe.min_gap, event cap); remover_path_id still resolves
```

### Removed before a rejection

`add_path()` compares the new path with every old path in turn.  It can
remove old paths that the new path dominates and then reach a later old
path that dominates the new one (the fuzzy cost comparison is not
transitive): the new path is rejected, and the old paths it removed are
gone from the pathlist too.  Each of those gets a `removed` event, right
after the remover's `rejected` event:

```text
displaced  removed by a new path that add_path() accepted;
           displacer_seq is that path's ACCEPTED event
removed    removed by a new path that add_path() then rejected;
           remover_seq is that path's REJECTED event
```

Both end the path's time in the pathlist: no later event of that path in
that rel follows, and an earlier SURVIVED snapshot does not make it a
survivor.  `removed` is recorded whether or not acceptance is logged.  The
core regression suite produces no such removal; injected candidates with
close costs do (test `pathprobe_graph`, section 8).  It needs a server
built with the current core patch, which defines
`HAVE_PATHPROBE_REMOVED_HOOK`; `coverage.add_path_removed` says whether the
point is observed.

`dominator_path_id` is exact.  `add_path()` compares the new path with the
paths in the pathlist in order (by disabled nodes, then total cost) and
stops at the first old path that dominates it, on fuzzy cost, pathkeys,
parameterization, rows and parallel safety; that path is the dominator.
Other paths later in the list might also dominate the new one; they are not
compared and not reported.  The dominator stays in the pathlist, so
`dominator_seq` is its ACCEPTED event into the same rel.

`gap_cost` of a REJECTED event is unchanged: it is still computed against
the rel's cheapest total path, which is usually not set while the rel is
being populated, so it is usually absent.  Do not read it as the distance
to the dominator.

## paths[]

One entry per `path_id` used in this result, including paths that have no
event of their own but are a child of one that has (for example a Memoize
or Material path built inside a nested loop).

```json
{"path_id": 12, "pathtype": "NestLoop", "rel": "{1,2}",
 "outer_path_id": 2, "inner_path_id": 9, "rows": 15}
{"path_id": 9, "pathtype": "BitmapHeapScan", "rel": "{2}", "subpath_id": 8,
 "rows": 3, "required_outer": "{1}"}
{"path_id": 8, "pathtype": "BitmapOr", "rel": "{2}", "child_path_ids": [6, 7]}
{"path_id": 6, "pathtype": "IndexScan", "rel": "{2}", "rows": 2,
 "required_outer": "{1}", "index_oid": 16403, "index_name": "orders_customer_idx"}
{"path_id": 17, "pathtype": "Append", "rel": "{1}", "child_path_ids": [1, 3],
 "rows": 40000}
```

Besides the edges, a node carries properties that do not change after the
Path is built, read when the node is registered:

```text
rows                the path's row estimate (per loop for a parameterized
                    path); omitted for BitmapAnd and BitmapOr, whose rows the
                    planner leaves at 0 ("not used"); can be 0 for a relation
                    proven empty
required_outer      the rels the path is parameterized by, as "{1}"; omitted
                    when the path is not parameterized
index_oid,          IndexScan, IndexOnlyScan, and a BitmapHeapScan over a
index_name          single index: the index and its name (looked up when the
                    JSON is rendered); omitted for other paths and for a
                    BitmapHeapScan over BitmapAnd / BitmapOr
hypothetical_index  true instead of index_oid and index_name when the index
                    is a pathprobe hypothetical index: its oid is a fake one
                    with no catalog entry and may equal a real relation's
                    oid; the hypo:<name> label of the path's events names it
```

```text
outer_path_id, inner_path_id  NestLoop, MergeJoin, HashJoin
subpath_id                    the single input of Projection, ProjectSet, Sort,
                              IncrementalSort, Material, Memoize, Gather,
                              GatherMerge, Limit, Unique, Agg, Group,
                              WindowAgg, LockRows, ModifyTable; for
                              SubqueryScan the subquery's own path; for
                              BitmapHeapScan the bitmap qual (an IndexScan
                              path, or a BitmapAnd / BitmapOr)
child_path_ids                Append, MergeAppend (subpaths), BitmapAnd,
                              BitmapOr (bitmap quals): all inputs, in the
                              path's own order; SetOp, RecursiveUnion: [left,
                              right].  Always present on these types; [] for
                              an Append with no inputs (a dummy rel)
```

Each path carries at most one kind of edge.  Other paths with inputs
(GroupingSets, ForeignScan's outer path, CustomScan's custom paths,
MinMaxAgg) carry no child edges.

A path appears in `paths[]` only if an event or a parent path refers to it.
Upper-rel paths (set operations, aggregation) have only ACCEPTED events, so
without `pathprobe.log_accepted` they and their children may be missing.
Paths of a subquery are planned with their own range table, so their `rel`
uses the subquery's numbering, not the outer query's.

## Path identity and graph edges

```text
path_id is local to one probe result: 1, 2, 3, ... in order of first sight
path_id is not stable across runs or between queries
path_id is not a pointer and does not encode one
seq is event order; path_id is path identity: one path can have several
  events (accepted, then survived in a snapshot, then displaced; or
  accepted again into an upper rel unchanged), all with the same path_id
```

Identity follows the Path object.  A wrapper built on top of a path (a
Sort, a Projection, a Gather) is a different path with its own id and a
`subpath_id` edge to the original.  An IndexPath can be both a scan path
in the pathlist and the bitmap qual of a BitmapHeapScan; it is the same
object, with one id.

Paths are not identified by cost or by `pathtype` + `rel`: several paths
share those.  While planning, pathprobe maps each Path object to its id.
`add_path()` frees the paths it rejects or displaces (except IndexPaths),
and the hooks drop those from the map, so a later path that reuses the
memory gets a new id.

Limits: with GEQO the planner frees join paths by resetting memory without
`add_path()`, so ids of join paths may be conflated there (a node-type and
parent-rel check guards against most cases); without the core hooks patch
pathprobe does not see any free, so the same holds for every path.
`paths[]` is in the JSON only; the text output and the `EXPLAIN`
integration do not show ids.  Paths removed before a rejection (above) are
freed after their `removed` event, and their ids are dropped like those of
displaced paths.  With an older copy of the core patch there is no such
event, their ids are not dropped and may be reused, and
`coverage.add_path_removed` is false.

The exact dominator needs a server built with the current core patch; it
defines `HAVE_PATHPROBE_REJECTED_DOMINATOR`.  An older copy of the patch
passes the rel's cheapest total path to the rejected hook; pathprobe does
not report that as a dominator, and the dominator fields are null.

## candidates[]

```json
{
  "pathname": "demo_scan",
  "relname": "t",
  "provider": "demo",
  "injected": true,
  "winner_forbidden": true,
  "cost_total": 1.0,
  "rows": 10,
  "fate": "displaced",
  "winner_strategy": "IndexScan",
  "winner_cost": 8.0,
  "gap": -7.0,
  "event_counts": {
    "accepted": 1,
    "rejected": 0,
    "survived": 1,
    "displaced": 1,
    "removed": 0
  }
}
```

`fate` is one of the values listed in `event-model.md`.

A tool should prefer `candidates[]` over any legacy single-candidate fields.
The legacy top-level `fate` object uses the same derivation as
`candidates[0].fate`; it is omitted when the call has no explicit candidate
(hypothetical indexes or advisor only).

## advisor

```json
{
  "enabled": true,
  "truncated": false,
  "atoms": "t: id=, v~",
  "baseline_cost": 59.0,
  "plan_cost": 4.05,
  "query_level_benefit": 54.95
}
```

`query_level_benefit = baseline_cost - plan_cost`.  A positive value means the
advisor candidate set improved this query's final plan cost.

## hypo_indexes[]

```json
{
  "name": "adv_3",
  "relname": "t",
  "ddl": "CREATE INDEX adv_3 ON t (id, v)",
  "injected": true,
  "in_plan": true,
  "accepted": 2,
  "rejected": 1,
  "survived": 1,
  "displaced": 0,
  "removed": 0,
  "best_cost": 4.05,
  "fate": "survived",
  "index_build": {
    "calls": {"plain": 1, "saop_bitmap": 0, "or_arm": 0},
    "generated": 1,
    "not_generated": []
  }
}
```

`in_plan=true` is final-plan attribution.  It is stronger than "this fake index
was generated".

Counts include events of a path carried unchanged into an upper rel; `fate`
uses only events whose planner relids equal those of the rel the index was
injected into.  `fate_reason` is present for
`not_examined` (`rel_not_planned`, `predicate_not_implied`) and `unknown`
(`index_build_not_covered`, `predicate_not_implied`, `outcome_not_observed`).
For `not_generated` the reasons are in `index_build.not_generated`:
`no_useful_input` (no clause, ordering, useful predicate or index-only scan),
`no_leading_clause` (AM without amoptionalkey, no clause on the first
column), `scantype_unsupported`.

`index_build` counts build_index_paths() calls for this index by call site
and is `null` when `coverage.index_build` is false.  Real indexes are not
reported.  See `event-model.md` for the fate alphabet.

## limits

```json
{
  "events_truncated": false,
  "summary_truncated": false,
  "max_events": 10000,
  "max_summary_entries": 128
}
```

If any truncation flag is true, an agent should not make a green decision.

## coverage

```json
{
  "survived_snapshot": true,
  "add_path_accepted": true,
  "add_path_rejected": true,
  "add_path_displaced": true,
  "add_path_removed": true,
  "add_path_precheck": true,
  "join_excluded": true,
  "index_build": false
}
```

Which decision points this run observes.  The absence of an event is
evidence only for a covered point: with `add_path_rejected=false`, no
`rejected` event does not mean nothing was rejected.  With
`index_build=false`, `not_generated` cannot be observed: a hypothetical
index with no events is `unknown(index_build_not_covered)`.

```text
add_path_accepted, add_path_displaced  core hooks patch and pathprobe.log_accepted
                                       (pathprobe_propose* turn it on)
add_path_rejected                      core hooks patch and pathprobe.min_gap = 0
add_path_removed                       core hooks patch with the removed hook
add_path_precheck                      core hooks patch
join_excluded                          pathprobe.enable_probe2
survived_snapshot                      always
index_build                            core hooks patch with the index path
                                       hook; pathprobe_propose* only, for
                                       hypothetical indexes
```

## summary[]

```json
{
  "rel": "{1,2}",
  "strategy": "HashJoin",
  "excluded": 0,
  "skipped": 0,
  "rejected": 0,
  "accepted": 2,
  "survived": 2,
  "best_cost": 19.06
}
```

Summary rows are useful for search-shape analysis.  They should not be used as a
replacement for labeled candidate events.  There is no displaced column; count
`displaced` events in `events[]`.

## Acknowledgements

`kept_reason` and the path metadata in `events[]` and `paths[]`
(`cost_startup`, `rows`, `required_outer`, `index_oid`, `index_name`,
`hypothetical_index`) were adapted from dashboard feedback and patches by
Dmitry Bulashev:
https://github.com/dbulashev/postgres/tree/pathprobe-image/.github/pathprobe/pathprobe-patches
