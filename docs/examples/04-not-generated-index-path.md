# 4. Not generated index path

## Question

A hypothetical index is injected and the plan does not use it.  Did the
planner build an index path and reject it, or never build one?

## Setup

`examples/04_not_generated_index_path.sql`: `ex4_ab (a, b, pad)`, 20000
rows, no indexes; the query filters only `b`.

```sql
SELECT * FROM ex4_ab WHERE b = 5;
```

Three hypothetical indexes:

```text
ab       btree (a, b)
btree_a  btree (a)
hash_a   hash (a)        (hypo_sql form, superuser only)
```

## What EXPLAIN shows

```text
 Seq Scan on ex4_ab
   Filter: (b = 5)
```

Nothing about indexes that do not exist.

## What pathprobe shows

```text
  hypo   | injected | acc | rej | surv |     fate      | build_calls | generated |     not_generated
---------+----------+-----+-----+------+---------------+-------------+-----------+-----------------------
 ab      | t        |   0 |   2 |    0 | rejected      | 1           | 1         | []
 btree_a | t        |   0 |   0 |    0 | not_generated | 1           | 0         | ["no_useful_input"]
 hash_a  | t        |   0 |   0 |    0 | not_generated | 1           | 0         | ["no_leading_clause"]
```

Text form:

```text
 hypo:ab      | fate=rejected
 hypo:btree_a | fate=not_generated(no_useful_input)
 hypo:hash_a  | fate=not_generated(no_leading_clause)
```

## Explanation

```text
Injected is not generated.
Generated is not accepted.
Accepted is not survived.
```

All three are `injected=true`.  Before the index path hook, `btree_a` and
`hash_a` looked like `ab` minus the rejections: injected, zero accepted,
rejected and survived.  Now each `build_index_paths()` call is reported
(`coverage.index_build = true`, `index_build.calls.plain = 1`):

- `hash_a`: hash needs a clause on its first column (`amoptionalkey` is
  false), and `b = 5` is not on `a`.  The call returns no path:
  `not_generated(no_leading_clause)`.
- `btree_a`: no clause, no useful ordering, no useful predicate, no
  index-only scan.  `not_generated(no_useful_input)`.
- `ab`: btree does not require a leading-column clause, so `b = 5` on the
  second column is enough to build paths.  They are generated, and
  rejected by `add_path()` in favour of the sequential scan.

## Caveat

- A btree on `(a, b)` with a filter on `b` alone is *not* a
  `no_leading_clause` case: btree has `amoptionalkey = true`.  That outcome
  needs an access method that requires a first-column clause, such as hash.
- Only hypothetical indexes are recorded; real indexes are not.
- The hook observes `build_index_paths()`.  An index skipped before that
  call (an unproven partial-index predicate) is `not_examined`, see
  example 5.
