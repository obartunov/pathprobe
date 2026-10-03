# 1. EXPLAIN is not enough

## Question

Which join strategies did the planner consider, and what happened to each?

## Setup

`examples/01_explain_is_not_enough.sql`: `ex1_customers` (1000 rows) and
`ex1_orders` (20000 rows, index on `customer_id`).

```sql
SET pathprobe.log_accepted = on;   -- record acceptances and displacements
SELECT * FROM ex1_orders o JOIN ex1_customers c ON c.id = o.customer_id
WHERE c.region = 3;
```

## What EXPLAIN shows

```text
 Hash Join
   Hash Cond: (o.customer_id = c.id)
   ->  Seq Scan on ex1_orders o
   ->  Hash
         ->  Seq Scan on ex1_customers c
               Filter: (region = 3)
```

One plan.  Nothing about merge or nested loop joins, or about the index on
`customer_id`.

## What pathprobe shows

Events at the join rel `{1,2}`, counted from `pathprobe_json()`:

```text
 strategy  | event     | n
-----------+-----------+----
 HashJoin  | accepted  |  1
 HashJoin  | skipped   |  1
 HashJoin  | survived  |  2
 MergeJoin | accepted  |  2
 MergeJoin | displaced |  2
 MergeJoin | rejected  |  1
 MergeJoin | skipped   |  3
 NestLoop  | accepted  |  1
 NestLoop  | displaced |  1
 NestLoop  | rejected  |  4
 NestLoop  | skipped   | 11
```

With `enable_mergejoin = off` the same query shows

```text
 MergeJoin | excluded |  2
```

## Explanation

All three join strategies competed.  Merge and nested loop joins were built
and accepted into the pathlist, then displaced; others were rejected by
`add_path()`, or skipped by `add_path_precheck()` before a Path was built.
Only the hash join survived.  A disabled strategy is reported as excluded
before generation, not silently absent.

The scan rels show the same: a parameterized `IndexScan` on `o` (usable as
the inner side of a nested loop) was accepted and survived at `{1}`, but no
join path using it won.

## Caveat

- Acceptance and displacement are recorded only with
  `pathprobe.log_accepted = on` (the propose functions turn it on); without
  it `coverage.add_path_accepted` is false and those events are absent.
- `skipped` comes from the precheck of join paths only.
- Event counts follow planner internals and can change between PostgreSQL
  versions; the example checks only their presence, not exact numbers.
