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
  "cost_total": 1.0,
  "disabled_nodes": 1
}
```

Rules:

```text
label should be present for proposed/hypothetical/advisor paths
origin is real or proposed
cost fields are omitted when not meaningful or when costs are suppressed
strings are JSON-escaped, never interpolated raw
```

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
    "displaced": 1
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
