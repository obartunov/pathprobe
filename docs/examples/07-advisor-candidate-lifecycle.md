# 7. Advisor candidate lifecycle

## Question

The advisor is on.  Which of its candidates mattered, and why did the
others not?

Advisor is not magic.  The advisor generates local, single-query index
candidates from the query's restrictions; pathprobe tests each candidate in
planner competition and reports what happened to it.

## Setup

`examples/07_advisor_candidate_lifecycle.sql`: `ex7_orders` (30000 rows,
primary key only).

```sql
SELECT * FROM ex7_orders WHERE customer_id = 42 AND status = 3 ORDER BY created;
-- spec: {"advise": true}
```

## What EXPLAIN shows

```text
 Sort
   Sort Key: created
   ->  Seq Scan on ex7_orders
         Filter: ((customer_id = 42) AND (status = 3))
```

## What pathprobe shows

The advisor block: what it extracted and the plan cost with all candidates
present.

```text
{"atoms": "ex7_orders: customer_id=, status=", "enabled": true,
 "baseline_cost": 731.03, "plan_cost": 15.21, "query_level_benefit": 715.82, ...}
```

One row per candidate, built from `hypo_indexes[]` by a helper in the script:

```text
 hypo  | ddl                                          | paths_built | acc | rej | disp | surv |   fate    | in_plan
-------+----------------------------------------------+-------------+-----+-----+------+------+-----------+--------
 adv_1 | ... (customer_id)                            |           1 |   2 |   0 |    2 |    1 | displaced | f
 adv_2 | ... (status)                                 |           1 |   0 |   2 |    0 |    0 | rejected  | f
 adv_3 | ... (customer_id, status)                    |           1 |   2 |   0 |    1 |    1 | survived  | t
 adv_4 | ... (customer_id, status) INCLUDE (id, ...)  |           1 |   0 |   2 |    0 |    0 | rejected  | f
 adv_5 | ... (created)                                |           1 |   1 |   1 |    0 |    1 | survived  | f
```

`coverage` has `index_build`, `add_path_accepted`, `add_path_displaced` and
`add_path_rejected` all true, so each of these columns is an observation,
not a default.

The displacement chain at the base relation:

```text
 seq |      path      |   event   |   label    | gap_cost
-----+----------------+-----------+------------+----------
   3 | IndexScan      | accepted  | hypo:adv_1 | 666.91
   4 | SeqScan        | displaced |            | 666.91
   5 | BitmapHeapScan | accepted  | hypo:adv_1 | 10.11
   6 | IndexScan      | displaced | hypo:adv_1 | 10.11
  10 | IndexScan      | accepted  | hypo:adv_3 | 37.90
  11 | BitmapHeapScan | displaced | hypo:adv_1 | 37.90
```

## Explanation

Read each row left to right:

```text
advisor proposed candidate   ddl (adv_N)
  -> injected                injected = true for all five
  -> IndexPath generated     paths_built = index_build.generated > 0
  -> add_path() outcome      acc / rej / disp / surv, summarized as fate
  -> final plan              in_plan
  -> coverage                which of the above could have been observed
```

- `adv_3 (customer_id, status)` survived and is the only candidate in the
  plan.  `query_level_benefit` belongs to the whole candidate set; `in_plan`
  says which candidate the plan uses.
- `adv_1 (customer_id)` was better than the sequential scan and was in the
  pathlist after its injection (`surv = 1`), then `adv_3` displaced both of
  its paths.
- `adv_2 (status)` built paths that lost outright.
- `adv_4` is the covering variant of `adv_3`.  Its best cost equals
  `adv_3`'s, and it was injected later, so `add_path()` kept the path it
  already had.  `rejected` here means "not better than an existing path",
  not "worse".
- `adv_5 (created)` was generated for the `ORDER BY`.  Its index path
  survived at the relation, and was rejected at the upper (ordered) level,
  where a sort over `adv_3` won.  The fate covers the index's relation;
  `in_plan = false` says the plan does not use it.

## Caveat

- The advisor is local to this one query; a candidate that helps here may
  hurt elsewhere.  pathprobe does not recommend creating any of these.
- Order of injection matters for ties (`adv_4`).
- `acc`, `rej` and the other counts include upper-rel events (`adv_5`'s
  `rej = 1`); the fate does not.
- Costs of hypothetical indexes are modeled: the AM estimator with a
  btree-shaped size estimate.
