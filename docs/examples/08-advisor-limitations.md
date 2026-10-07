# 8. Advisor limitations

## Question

What does the advisor not propose, and what does a surviving hypothesis
not prove?

## Setup

`examples/08_advisor_limitations.sql`: `ex8_customers` (5000 rows),
`ex8_orders` (30000 rows, an existing index on `status`).  Each query is
probed with `{"advise": true}`; the last one with a hand-written hypothetical
index instead.

## What EXPLAIN shows

For the join `ex8_orders o JOIN ex8_customers c ON c.id = o.customer_id
WHERE c.region = 3`: a hash join over two sequential scans.  Nothing about
indexes that could exist.

## What pathprobe shows

```text
A  join, filter on c.region
   atoms: "ex8_orders:  | ex8_customers: region="
   adv_1 ex8_customers (region)                  survived  in_plan   benefit 53.15
   adv_2 ex8_customers (region) INCLUDE (...)    rejected

B  total IS NOT NULL AND customer_id = 9
   atoms: "ex8_orders: customer_id="
   adv_1 ex8_orders (customer_id)                survived  in_plan

C  status = 3 OR total = 7
   atoms: "ex8_orders: (((status = 3) OR (total = 7)))="
   adv_1 ex8_orders ((((status = 3) OR (total = 7))))   rejected   benefit 0.00

D  status = 3 (an index on status exists)
   adv_1 ex8_orders (status) INCLUDE (...)       rejected            benefit 0.00

E  join, hand-written hypothesis on ex8_orders (customer_id)
   orders_customer   fate survived   in_plan false
   advisor.enabled false, advisor.query_level_benefit 0.00
```

## Explanation

```text
Advisor proposes hypotheses.  PathProbe measures what happened to them.
A hypothesis that is never proposed is never measured.
```

- A: join clauses are not modeled.  No candidate on the join column
  `ex8_orders.customer_id` is generated, and the atoms for `ex8_orders` are
  empty.
- B: `IS NOT NULL` is skipped silently; the output does not list skipped
  clauses.
- C: an `OR` is not split into branches.  The whole clause becomes one
  boolean expression atom, and its candidate loses.
- D: the advisor never proposes a candidate with the same keys as an
  existing index (`advise_skip_prefix` also drops existing prefixes).  The
  covering variant loses; a benefit of 0 is evidence that the existing
  index already serves this query.
- E: the user can supply the missing hypothesis with `hypo_indexes`.  The
  join-column index survives at its own relation (its index scans stay in
  the relation's pathlist), but the plan stays a hash join.
  `survived` is about the index's relation; only `in_plan` is about the
  final plan.

## Caveat

- The `advisor` block is present, with `enabled = false`, when only
  hypothetical indexes are given; there `query_level_benefit` is the
  benefit of the hand-written set, not of advisor candidates.
- `atoms` is a display string, not a structured list; it is not meant for
  parsing.
- See `docs/user/advisor.md` for the shapes the advisor handles.
