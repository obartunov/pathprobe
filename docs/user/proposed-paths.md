# Proposed paths

> **Access.**  Superusers get full behavior.  Non-superusers run in a
> restricted ACL-aware mode: a single `SELECT` probe over relations and columns
> the caller may already `SELECT` (all query levels authorized under the
> caller's privileges; RLS and view/rule base exposure fail closed).  See the
> README security model.

Proposed paths let a user or tool inject a diagnostic candidate into real
PostgreSQL planner competition.

The candidate is not a production execution method.  It is a what-if path for
planning evidence.

## Basic form

```sql
SELECT pathprobe_propose_json(
  $$ SELECT * FROM t WHERE id = 42 $$,
  '{
     "target": {"relname": "t"},
     "candidate": {"pathname": "demo_scan", "provider": "demo"},
     "cost": {"startup_cost": 0.1, "total_cost": 10.0, "rows": 10},
     "mode": {"winner_forbidden": true}
   }'
)::jsonb;
```

Important fields:

```text
target.relname       base relation targeted by the candidate
candidate.pathname   candidate identity; must be unique in multi-candidate calls
candidate.provider   human/tool label
cost.total_cost      cost used in planner competition
planner.parallel_safe default true; add_path() never rejects a parallel-safe
                     path in favour of a non-parallel-safe one, so set false
                     to compete on cost alone against non-parallel-safe paths
mode.winner_forbidden prevent diagnostic candidate from becoming executable winner
```

## Candidate fate

`pathprobe_propose_json()` returns `candidates[]`.  Each candidate gets a finite
fate:

```text
target_not_found         no relation with the target name in the query
not_examined             target in the query, never reached the injection point
rejected                 built and rejected by add_path()
displaced                entered the pathlist, later pushed out by a better path
survived                 in the pathlist after its add_path(), not displaced
survived_at_point        in the pathlist after its add_path(); displacement
                         not observable in this run
survived_upper           the candidate path reached an upper relation
unknown                  injected, no outcome observed
unknown_after_accept     accepted, later outcome not observed
inconsistent             events contradict an observed decision point
```

Three of these have no events at all and still mean different things:

```text
target_not_found  spec/identity failure: the spec names no relation of the query
not_examined      the target is known, but planning never reached the point
                  where the candidate is injected
unknown           the candidate was injected; the decision point that would
                  report its outcome is not covered in this run
```

A fate is never inferred from a missing event; see `coverage` in
`docs/technical/json-format.md` and
[example 3](../examples/03-target-not-found-not-examined-unknown.md).  For
tools, `inconsistent` is not a normal outcome: it means either a probe bug or
an unmodeled planner action.

## Policy boundary

`winner_forbidden=true` keeps the candidate diagnostic-only by using the planner
competition but preventing the proposed path from winning the executable plan.

`winner_forbidden=false` is useful only as a boundary test.  If a proposed path
wins, execution must stop with an explicit error.  Silent execution would be a
bug.

## Multi-candidate form

```sql
SELECT pathprobe_propose_json(
  $$ SELECT * FROM t JOIN s ON t.id = s.id WHERE t.id = 42 $$,
  '{"candidates": [
     {
       "target": {"relname": "t"},
       "candidate": {"pathname": "t_fast", "provider": "demo"},
       "cost": {"total_cost": 1.0, "rows": 10},
       "mode": {"winner_forbidden": true}
     },
     {
       "target": {"relname": "s"},
       "candidate": {"pathname": "s_fast", "provider": "demo"},
       "cost": {"total_cost": 2.0, "rows": 10},
       "mode": {"winner_forbidden": true}
     }
   ]}'
)::jsonb;
```

A tool should read the `candidates[]` array, not the legacy single-candidate
view.

## Pathkeys claim

A candidate may claim output order:

```json
{"planner": {"pathkeys": ["id"]}}
```

The claim does not invent new PostgreSQL `PathKey`s.  It matches the query's
own canonical pathkeys where possible.  Direction comes from the query.

## Fate and the upper rel

`survived` means the candidate is still on the base-rel frontier.
`survived (reached upper rel)` (`survived_upper` in JSON) means the
CustomPath itself was re-accepted at an upper planning level - for an
`ORDER BY` query this is the direct observable effect of a matching
`pathkeys` claim: the candidate enters the ordered competition as-is,
an unordered twin only survives under a Sort.  Without query pathkeys
most surviving candidates reach the upper rel through the scan/join
target re-add, so the marker is a pathkeys signal only when the query
has an ordering.
