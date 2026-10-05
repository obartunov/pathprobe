# 2. Displaced paths

## Question

A path was accepted by `add_path()`.  Is that its final outcome?

## Setup

`examples/02_displaced_paths.sql`: the join of example 1 (`ex2_*` tables),
and two proposed candidates on `ex2_orders`.

## What EXPLAIN shows

The hash join.  Paths that were accepted and later removed leave no trace.

## What pathprobe shows

Acceptances and displacements at the join rel, in planner order:

```text
 seq |   path    |   event   | path_id | displacer | displacer_seq | displacer_type | gap_cost
-----+-----------+-----------+---------+-----------+---------------+----------------+----------
  15 | MergeJoin | accepted  |       9 |           |               |                |
  18 | NestLoop  | accepted  |      11 |           |               |                | 773.13
  19 | MergeJoin | displaced |       9 |        11 |            18 | NestLoop       | 773.13
  25 | MergeJoin | accepted  |      17 |           |               |                | 199.18
  26 | NestLoop  | displaced |      11 |        17 |            25 | MergeJoin      | 199.18
  28 | HashJoin  | accepted  |      19 |           |               |                | 526.30
  29 | MergeJoin | displaced |      17 |        19 |            28 | HashJoin       | 526.30
  30 | HashJoin  | survived  |      19 |           |               |                |
```

The text form says the same:

```text
#18   p1  {1,2}         NestLoop                 1105.96  impr_by=+773.13         accepted
#19   p1  {1,2}         MergeJoin                1879.09  impr_by=+773.13         displaced
```

Two proposed candidates on one relation, the cheaper one injected second:

```text
 candidate |      fate      | event_counts
-----------+----------------+---------------------------------------------------------------
 early     | displaced      | {"accepted": 1, "rejected": 0, "survived": 1, "displaced": 1}
 late      | survived_upper | {"accepted": 2, "rejected": 0, "survived": 1, "displaced": 0}
```

## Explanation

A DISPLACED event is emitted from the list of paths `add_path()` removes
when it accepts a new one, right after that ACCEPTED event.  It names the
displaced path by `path_id` (the same id as its own earlier ACCEPTED event)
and the displacer by `displacer_path_id`, `displacer_seq` and
`other_pathtype`.  `gap_cost` on both is `old.total_cost - new.total_cost`:
how much better the displacer is.  The two merge join paths are different
paths (ids 9 and 17), which the event order alone does not show.

`early` was in the pathlist right after its own `add_path()` (one
`survived` event), and was displaced afterwards.  A fate derived from "no
SURVIVED event seen" or from that snapshot would be wrong both ways; the
fate is `displaced` because the displacement is observed.

## Caveat

- `path_id` is local to one result and not stable across runs.
- Displacement needs `pathprobe.log_accepted = on`
  (`coverage.add_path_displaced`).
- Paths dropped when `apply_scanjoin_target_to_paths()` rebuilds a
  pathlist produce no DISPLACED event.  This is a known gap.
