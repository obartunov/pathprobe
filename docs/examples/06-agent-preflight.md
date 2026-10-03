# 6. Agent preflight

## Question

An automated system (an SQL agent, a migration checker, a CI job) wants to
look at a query before running it.  What can it read from pathprobe that it
cannot read from EXPLAIN?

## Setup

`examples/06_agent_preflight.sql`: `ex6_events` (30000 rows, primary key
only) and two helper functions written in SQL over the JSON:

```text
ex6_evidence(query)       from pathprobe_json(): coverage gaps, truncation,
                          base rels where only a SeqScan survived,
                          excluded strategies, skipped and displaced counts
ex6_hypo(query, spec)     from pathprobe_propose_json(): fate and in_plan of
                          candidate indexes
```

Neither function decides anything.

## What EXPLAIN shows

For `SELECT * FROM ex6_events WHERE account = 42`: a sequential scan.  For
the self-join with `enable_mergejoin = off`: a plan without merge joins, and
no hint that a setting removed them.

## What pathprobe shows

```text
evidence  {"skipped": 0, "excluded": [], "displaced": 0, "truncated": false,
           "seqscan_only": ["ex6_events"], "coverage_gaps": ["index_build"]}

remedies  {"hypo": [{"fate": "survived",         "name": "by_account", "in_plan": true},
                    {"fate": "not_generated",    "name": "by_kind",    "in_plan": false},
                    {"fate": "target_not_found", "name": "typo",       "in_plan": false}],
           "index_build_covered": true}

evidence  {"skipped": 4, "excluded": ["MergeJoin"], "displaced": 3,
(self-    "truncated": false, "seqscan_only": ["a"],
 join)     "coverage_gaps": ["index_build"]}
```

## Explanation

Signals a preflight step can use:

```text
coverage_gaps       what this run could not observe; absent events there
                    are not evidence (plain pathprobe_json() never covers
                    index_build)
truncated           the event log hit its cap: do not treat as complete
seqscan_only        no other access path survived for that relation
excluded            strategies removed before generation (enable_* GUCs,
                    pg_plan_advice)
hypo fate           which candidate index would be built, and used
target_not_found    a spec that names no relation of the query, e.g. a typo
```

The script ends with an illustrative policy:

```text
truncated                                   -> defer
seqscan_only and a hypo index survived
  and is in the plan                        -> suggest index, then run
seqscan_only                                -> run with statement_timeout
otherwise                                   -> run
```

For the first query it says `suggest index, then run`.

This is evidence for a preflight decision, not an automatic decision.
pathprobe provides structured planner evidence; the policy, its thresholds
and the relation sizes it needs are outside pathprobe.

## Caveat

- `seqscan_only` is planner evidence, not a cost or a row count; a gate
  still needs relation size or plan cost to call a scan "large".
- `rels` maps range-table indexes to aliases (`a` in the self-join), not
  to relation names.
- `pathprobe.log_accepted` is superuser-only; without it acceptance and
  displacement are coverage gaps too.
- See `docs/user/agent-gate.md` for the agent-mode limits
  (`pathprobe.agent_mode`, `max_events`).
