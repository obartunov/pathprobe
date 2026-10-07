# 9. Advisor audit

## Question

For common query patterns, what does the advisor generate, and what does
pathprobe measure for each candidate?

The current advisor is a local candidate generator.  PathProbe measures what
happened to the generated candidates.  It does not claim that a candidate is
globally good for the workload.

## Setup

`examples/09_advisor_audit.sql`: `ex9_t` (30000 rows, primary key, an index
on `(b, a)` and one on `(lower(d))`) and `ex9_u` (3000 rows).  Twenty-three
queries, each probed with `{"advise": true}`.  For each candidate: the atoms
the advisor extracted, the candidate's keys, whether index paths were built,
its fate and `in_plan`.

## What EXPLAIN shows

The plan with the indexes that exist.  It cannot say which index the advisor
would propose, or what happened to it.

## What pathprobe shows

A selection of rows, cells abbreviated:

```text
 n | pattern                  | atoms                         | candidate              | built | fate      | in_plan
---+--------------------------+-------------------------------+------------------------+-------+-----------+--------
 3 | equality + range         | ex9_t: c=, ts~                | ex9_t (c, ts)          |     1 | survived  | t
 3 | equality + range         | ex9_t: c=, ts~                | ex9_t (c)              |     1 | displaced | f
 4 | ORDER BY ... LIMIT       | ex9_t:                        | ex9_t (ts)             |     1 | survived  | t
 5 | ORDER BY ... DESC LIMIT  | ex9_t:                        |                        |       |           |
 7 | join, no constant on t   | ex9_u: id= | ex9_t:           | ex9_u (id) INCLUDE ... |     1 | rejected  | f
19 | join, constant via EC    | ex9_u: w= | ex9_t: c=         | ex9_t (c)              |     1 | survived  | t
 9 | OR, one column           | ex9_t:                        |                        |       |           |
13 | expression, index exists | ex9_t: (lower(d))=            | ex9_t ((lower(d)))     |     1 | rejected  | f
15 | keys of (b, a) reversed  | ex9_t: a=, b=                 | ex9_t (a, b)           |     1 | rejected  | f
17 | LIKE prefix              | ex9_t:                        |                        |       |           |
18 | eight restrictions       | ex9_t: a=, b=, c=, d=, ts=, id= | ex9_t (a, b, c, d, ts, id) | 1 | rejected | f
20 | NOT boolean column       | ex9_t: c=, flag=              | ex9_t (flag)           |     1 | rejected  | f
21 | AND inside OR            | ex9_t:                        |                        |       |           |
22 | boolean expression       | ex9_t:                        |                        |       |           |
23 | boolean function         | ex9_t: (isfinite(ts))=        | ex9_t ((isfinite(ts))) |     1 | rejected  | f
```

## Explanation

- Simple equality, range, equality + range, `IS NULL`, expressions,
  boolean columns, `ORDER BY ... LIMIT` and `GROUP BY` produce the expected
  candidates, and each traces through `index_build`, the `add_path()`
  counts and `in_plan`.
- `ORDER BY ... DESC`, `IS NOT NULL`, `LIKE 'prefix%'` and a bare join
  clause produce nothing.  A constant that reaches a join column through an
  equivalence class does (19).
- An `OR` produces no atom and no candidate (8, 9, 21), also when all its
  branches test one column: `ts = x OR ts = y` gets no `(ts)` candidate.
  When the audit was written the whole OR became one boolean expression
  candidate, `ex9_t ((((ts = ...) OR ...)))`; that was fixed.
- A boolean column is an atom, also under `NOT` (14, 20), and so is a
  function returning boolean (23); another boolean expression, such as
  `(a > b) IS TRUE`, is not (22).
- An expression index that already exists is proposed again (13), and
  `(a, b)` is proposed next to an existing `(b, a)` (15).  Neither wins:
  a hypothetical btree index costs the same as an identical real one, and
  the planner keeps the real one.  When the audit was written they did win
  `in_plan`, by the descent cost the hypothetical index was missing (0.25
  here), as did the six-column composite next to the primary key (18) and
  the covering variant of `ex9_u`'s primary key (7); that was fixed.
- Of eight restrictions only six become atoms (18); nothing reports the two
  that were dropped.

[The audit](../technical/advisor-audit.md) has the full pattern table, the
classification of each behavior, and the bugs found.

## Caveat

- `built` is always 1 here: the advisor only proposes keys taken from
  indexable clauses or pathkeys, so its candidates are not `not_generated`.
- `in_plan = true` for a candidate that duplicates an existing index
  would now point at a costing difference other than the tree height, for
  example a page estimate; it is not evidence of a useful index.
- The checks in the script assert what the advisor generates, not which
  candidate wins.
