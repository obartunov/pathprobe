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
  "fate": "accepted_not_survived",
  "winner_strategy": "IndexScan",
  "winner_cost": 8.0,
  "gap": -7.0,
  "event_counts": {
    "accepted": 1,
    "rejected": 0,
    "survived": 0
  }
}
```

A tool should prefer `candidates[]` over any legacy single-candidate fields.

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
  "best_cost": 4.05
}
```

`in_plan=true` is final-plan attribution.  It is stronger than "this fake index
was generated".

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
replacement for labeled candidate events.
