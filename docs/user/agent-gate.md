# Planner-aware gate for SQL agents

A SQL agent should not execute every generated query immediately.  It should
first ask PostgreSQL how hard the query looks and whether there is a safer path.

`pathprobe` provides planner evidence for that decision.

## Flow

```text
agent builds SQL
      ↓
pathprobe_propose_json(..., advisor/proposed/hypo spec)
      ↓
JSON evidence
      ↓
deterministic policy
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

## Fields an agent should read

```text
limits.events_truncated
limits.summary_truncated
advisor.enabled
advisor.truncated
advisor.query_level_benefit
hypo_indexes[].in_plan
hypo_indexes[].ddl
candidates[].fate
candidates[].gap
candidates[].event_counts
summary[].accepted / rejected / skipped / survived
```

## Minimal deterministic policy

```text
GREEN
  no truncation
  low plan cost
  no dominant in_plan advisor candidate
  execute normally

YELLOW
  moderate cost or contested frontier
  execute with statement_timeout, or route to replica

RED
  truncation occurred
  very high cost
  dominant advisor index is in_plan
  many join-pressure events
  refuse/defer/suggest index instead of running on primary
```

This is deliberately simple.  The LLM may propose a query, but the gate decision
should be based on machine-readable evidence.

## Boundary

```text
LLM / agent  proposes or asks
pathprobe    measures real planner competition
policy       decides using deterministic rules
PostgreSQL   executes only if policy allows it
```

Do not let an agent parse human-readable tables.  Human output is for people.
JSON is for tools.
