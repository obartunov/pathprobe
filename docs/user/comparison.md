# Comparison with existing PostgreSQL tools

`pathprobe` is not a replacement for `EXPLAIN`, `auto_explain`,
`pg_stat_statements`, HypoPG, `pg_hint_plan`, or `pg_plan_advice`.  Those
tools are useful and should stay in the PostgreSQL performance toolbox.
`pathprobe` answers a different question.

## The short version

Existing tools mostly show one of these:

```text
EXPLAIN             the final selected plan
auto_explain        automatic logging of plans for slow statements
pg_stat_statements  workload-level planning/execution statistics
HypoPG              hypothetical indexes visible to the planner/EXPLAIN
pg_hint_plan        user-supplied hints to influence plans
pg_plan_advice      advice that reproduces or alters planner decisions
```

`pathprobe` shows the planner path lifecycle:

```text
which paths were considered
which paths were skipped
which paths were rejected
which paths entered the frontier
which accepted paths were later displaced
which paths survived
whether a hypothetical index produced any index path at all
which proposed / hypothetical / advisor candidates affected the decision
```

The key sentence: existing tools mostly show the final plan, workload
statistics, hypothetical objects, or hint-controlled plans.  `pathprobe`
shows how planner alternatives competed before the final plan existed.

## EXPLAIN

`EXPLAIN` shows the plan the optimizer selected: the plan tree, estimated
costs and rows, plan nodes.  Essential, but final-plan oriented.

`pathprobe` is earlier and more diagnostic - it records planner path events
while the optimizer is still exploring alternatives.

```text
EXPLAIN    what plan did PostgreSQL choose?
pathprobe  what happened to competing paths before the final plan existed?
```

`EXPLAIN` may show that a Seq Scan survived into the final plan.  `pathprobe`
may also show that an IndexPath was generated and rejected, or accepted and
later displaced, that a join path was skipped by precheck, or that a proposed
CustomPath survived only when `winner_forbidden=true`.

## auto_explain

`auto_explain` logs execution plans for slow statements automatically -
which slow queries produced which plans.  `pathprobe` answers how the
planner's internal path competition behaved for a query, which an agent may
need *before* execution to decide: run now, run on a replica, set a timeout,
ask for an index, refuse or defer.

## pg_stat_statements

`pg_stat_statements` is workload-level: it aggregates planning and execution
statistics across statements, excellent for finding query families that
matter.  It does not expose path-level competition for one planning attempt.

They are complementary: use `pg_stat_statements` to find a problematic query,
then `pathprobe` to inspect the planner path lifecycle for it.

## HypoPG

HypoPG is the closest tool in spirit for hypothetical indexes: create virtual
indexes and check whether PostgreSQL would use them.  `pathprobe` does not
replace that idea; it ties a hypothetical candidate to path events - was it
injected, was an index path generated at all, was it accepted, rejected or
displaced, did it appear in the final plan, what was the query-level benefit.

HypoPG can show whether a hypothetical index appears in a final plan.
pathprobe can also show that a hypothetical index was examined but generated
no IndexPath (`not_generated`, with a reason such as `no_leading_clause`),
which needs the core patch with the index path hook.  It does not do this for
real indexes, and an index skipped before path generation (an unproven
partial predicate) is reported as `not_examined`, not `not_generated`.

For agent workflows that evidence matters: an agent should not merely say
"create this index" but show that the candidate changed planner competition.

## pg_hint_plan

`pg_hint_plan` influences plans through hints - a control mechanism.
`pathprobe` is an observation and decision-support mechanism, and should not
become a hinting system.

```text
pg_hint_plan  force or influence the planner
pathprobe     observe planner alternatives and test candidates safely
```

## pg_plan_advice

`pg_plan_advice` (contrib, PostgreSQL master) generates and enforces plan
advice: scan methods, join order, join methods.  It enforces advice by
restricting the plan strategies the planner may use for a given relation or
join (`pgs_mask`).

```text
pg_plan_advice  reproduce or alter planner decisions
pathprobe       observe how alternatives competed under those decisions
```

Both can be loaded in the same planning cycle.  How advice shows up in
`pathprobe` depends on where the restriction applies:

- a disabled scan strategy is still costed, so its paths appear with
  `disabled_nodes > 0`, and join paths built on them are skipped with
  `skip_reason: disabled`;
- a disabled join method is usually not generated at all; `pathprobe`
  reports the blocked join family as `excluded`.

`pathprobe` reports the effect, not its source: `disabled` and `excluded` look
the same whether the restriction came from `pg_plan_advice`, an `enable_*`
setting, or another extension.  `excluded` is also coarse: it is emitted only
when a whole join family (nested loop, merge, hash) is blocked for a join, so
disabling a single variant such as memoize or materialize does not produce it.

## Why we wrote pathprobe

Final plans are not enough for planner-aware agents.  An agent that generates
SQL needs to ask whether a query is likely dangerous, whether cheaper
alternatives existed, whether the planner considered an index path, whether a
hypothetical index would materially change the plan, whether a proposed
custom path really survived competition, whether the planner search itself is
growing too large, and whether to execute, route, delay, or reject.

Existing tools answer parts of this; none provides a single machine-readable
path lifecycle model:

```text
planner events
  -> path lifecycle
  -> JSON evidence
  -> advisor / hypothetical / proposed candidates
  -> evidence for a decision made by a policy outside pathprobe
```

The goal is not to replace PostgreSQL's optimizer, but to make its decisions
observable enough that tools and agents can act responsibly.
