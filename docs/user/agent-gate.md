# Planner evidence for agent preflight

A SQL agent, a migration checker or a CI job may want to look at a query before
running it.  `pathprobe` provides structured planner evidence for that step:
evidence for a deterministic policy.  It does not provide the policy, and it is
not itself a gate.

## Flow

```text
agent builds SQL
      ↓
pathprobe_json(...) / pathprobe_propose_json(..., advisor/proposed/hypo spec)
      ↓
JSON evidence
      ↓
deterministic policy (outside pathprobe)
      ↓
execute / route / defer / refuse / suggest index
```

## Conservative profile

```sql
SET pathprobe.agent_mode = on;
SET pathprobe.max_events = 10000;
SET pathprobe.max_advisor_candidates = 16;
SET pathprobe.max_hypo_indexes = 64;
```

Then:

```sql
SELECT pathprobe_propose_json(
  $$ SELECT * FROM orders WHERE customer_id = 42 ORDER BY order_date $$,
  '{"advise": true, "advise_skip_prefix": true}'
)::jsonb;
```

## Fields a policy can read

```text
coverage                     which decision points were observed; an absent
                             event is evidence only for a covered point
limits.events_truncated      the event log hit its cap: evidence incomplete
limits.summary_truncated
advisor.enabled
advisor.truncated
advisor.query_level_benefit
hypo_indexes[].fate          not_examined / not_generated / rejected /
                             displaced / survived / unknown ...
hypo_indexes[].index_build   build_index_paths() calls and outcome, or null
hypo_indexes[].in_plan
hypo_indexes[].ddl
candidates[].fate
candidates[].gap
candidates[].event_counts
summary[].accepted / rejected / skipped / survived / excluded
events[] with kind excluded  join strategies removed before generation
```

`unknown`, `unknown_after_accept` and `inconsistent` fates, and any truncation,
are not green: they mean the evidence does not decide the question.

## An illustrative policy

The rules below show the shape of a policy over this evidence.  They are an
example, not a recommendation and not part of `pathprobe`; thresholds, plan
cost limits and relation sizes come from the operator.

```text
GREEN
  no truncation, no coverage gap that matters for the question
  low plan cost
  execute normally

YELLOW
  moderate cost or contested frontier
  execute with statement_timeout, or route to replica

RED
  truncation occurred
  very high cost
  a hypothetical index survives and is in_plan with a large benefit
  many join-pressure events
  refuse/defer/suggest index instead of running on primary
```

[Example 6](../examples/06-agent-preflight.md) computes such evidence in SQL
and applies a small policy of this kind.

## Boundary

```text
LLM / agent  proposes or asks
pathprobe    reports what the planner did, and what it did not observe
policy       decides using deterministic rules
PostgreSQL   executes only if policy allows it
```

Do not let an agent parse human-readable tables.  Human output is for people.
JSON is for tools.
