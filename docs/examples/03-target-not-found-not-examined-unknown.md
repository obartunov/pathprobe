# 3. target_not_found, not_examined, unknown

## Question

A proposed candidate has no events at all.  Was it never built, never
reached, or just not observed?

## Setup

`examples/03_target_not_found_not_examined_unknown.sql`: `ex3_orders`,
`ex3_customers`, and one proposed candidate, more expensive than the primary
key scan, aimed at three targets:

```text
1  nosuchtable     SELECT * FROM ex3_orders WHERE id = 7
2  ex3_customers   SELECT o.* FROM ex3_orders o
                     LEFT JOIN ex3_customers c ON c.id = o.customer_id
                   WHERE o.id = 7
3  ex3_orders      SELECT * FROM ex3_orders WHERE id = 7
```

## What EXPLAIN shows

For query 2:

```text
 Index Scan using ex3_orders_pkey on ex3_orders o
   Index Cond: (id = 7)
```

`ex3_customers` is gone (join removal).  EXPLAIN cannot say whether a
candidate on it lost or never had a chance.

## What pathprobe shows

```text
 n |    target     |       fate       | injected | event_counts
---+---------------+------------------+----------+-----------------------------------------------------------------------------
 1 | nosuchtable   | target_not_found | false    | {"removed": 0, "accepted": 0, "rejected": 0, "survived": 0, "displaced": 0}
 2 | ex3_customers | not_examined     | false    | {"removed": 0, "accepted": 0, "rejected": 0, "survived": 0, "displaced": 0}
 3 | ex3_orders    | rejected         | true     | {"removed": 0, "accepted": 0, "rejected": 1, "survived": 0, "displaced": 0}
```

Case 3 again, with `pathprobe.min_gap = 1000000`:

```text
 rejection_covered |  fate   | event_counts
-------------------+---------+-----------------------------------------------------------------------------
 false             | unknown | {"removed": 0, "accepted": 0, "rejected": 0, "survived": 0, "displaced": 0}
```

## Explanation

Cases 1, 2 and the last one have identical (empty) event counts and three
different meanings:

```text
target_not_found  the spec names no relation of the query; a spec error
not_examined      the relation is in the query, but planning never reached
                  the point where the candidate is injected
unknown           the candidate was injected; the decision point that would
                  report its outcome is not observed in this run
```

With `min_gap` set, the rejected hook drops events whose gap is below it,
and `coverage.add_path_rejected` is false.  The same rejected candidate is
then `unknown`, not `rejected` and not `survived`.  Missing observation is
not a planner state.

## Caveat

- `pathprobe.min_gap` is the only way to reduce coverage in a full build;
  it is used here only to make the gap visible.
- Description only, not run by the script: on a server without the core
  hooks patch only `survived_snapshot` (and `join_excluded`) are covered.
  A candidate that loses there has no event at all and its fate is
  `unknown`, never `rejected`.  On a server with an older copy of the patch
  (no index path hook), `coverage.index_build` is false, and a hypothetical
  index without events is `unknown(index_build_not_covered)` rather than
  `not_generated`.
