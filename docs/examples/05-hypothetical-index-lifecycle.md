# 5. Hypothetical index lifecycle

## Question

`injected=true` says only that pathprobe offered the index to the
planner.  What happened to it?

## Setup

`examples/05_hypothetical_index_lifecycle.sql`: `ex5_h (id, a, b, pad)`,
20000 rows, autovacuum off and `VACUUM (ANALYZE)` so that index-only scan
costs do not depend on autovacuum timing.  Four probes:

```text
A  SELECT * FROM ex5_h WHERE a = 7         on_a (a), on_b (b),
                                           hash_b hash (b),
                                           part_a (a) WHERE b = 5
B  SELECT * FROM ex5_h WHERE b = 5         on_a_b (a, b)
C  SELECT a, b FROM ex5_h WHERE a = 7      narrow (a), then covering (a, b)
D  SELECT * FROM ex5_h WHERE a = 7 OR b = 9  or_a_b (a, b)
```

## What EXPLAIN shows

For A: `Seq Scan on ex5_h`, `Filter: (a = 7)`.  Hypothetical indexes are
invisible to it.

## What pathprobe shows

```text
   hypo   |     fate      |        reason         | calls (plain/or_arm) | not_generated         | acc | rej | surv | disp | in_plan
----------+---------------+-----------------------+----------------------+-----------------------+-----+-----+------+------+--------
A on_a    | survived      |                       | 1 / 0                | []                    |   3 |   0 |    1 |    1 | t
A on_b    | not_generated |                       | 1 / 0                | ["no_useful_input"]   |   0 |   0 |    0 |    0 | f
A hash_b  | not_generated |                       | 1 / 0                | ["no_leading_clause"] |   0 |   0 |    0 |    0 | f
A part_a  | not_examined  | predicate_not_implied | 0 / 0                | []                    |   0 |   0 |    0 |    0 | f
B on_a_b  | rejected      |                       | 1 / 0                | []                    |   0 |   2 |    0 |    0 | f
C narrow  | displaced     |                       | 1 / 0                | []                    |   2 |   0 |    1 |    2 | f
C covering| survived      |                       | 1 / 0                | []                    |   2 |   1 |    1 |    0 | t
D or_a_b  | unknown       | outcome_not_observed  | 1 / 2                | ["no_useful_input"]   |   0 |   0 |    0 |    0 | f
```

(Condensed from the script output; every row is `injected=true`.)

## Explanation

- `part_a`: the partial predicate `b = 5` is not implied by `a = 7`, so
  `create_index_paths()` skips the index before `build_index_paths()`.
  Zero build calls: `not_examined(predicate_not_implied)`, not
  `not_generated`.
- `on_b`, `hash_b`: examined, no path built (see example 4).
- `on_a_b`: paths built and all rejected.
- `narrow`: in the pathlist right after its own injection (`surv = 1`),
  then every accepted path of it is displaced by `covering`
  (`disp = acc = 2`).  Its fate is `displaced`; the snapshot alone would
  have said "survived".
- `or_a_b`: the plain call builds nothing (`no_useful_input`); the two
  OR-arm calls build bitmap index paths.  Those end up only inside a
  BitmapOr, whose arms carry no hypo label, so no `add_path()` outcome is
  attributed to the index.  `unknown(outcome_not_observed)` is the honest
  answer; claiming `rejected` or `not_generated` would be wrong.
- `in_plan` is separate: it says the index is in the final plan.  `on_a`
  survived and is in the plan; `narrow` was accepted and is not.

## Caveat

- Fate uses only events at the relation the index was injected into;
  `acc` and the other counts also include a path carried unchanged into an
  upper rel (that is why `on_a` shows `acc = 3`).
- Hypothetical indexes are injected one after another and compete with
  each other; order matters for C.
- `hash_b` and `part_a` use the `hypo_sql` form, which is superuser-only.
