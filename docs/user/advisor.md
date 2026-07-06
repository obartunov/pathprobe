# Advisor and hypothetical indexes

> The advisor runs through `pathprobe_propose_json()` and is **superuser-only**
> in this release (a conservative safety gate; ACL-aware access is on the
> roadmap).  A non-superuser call returns
> `ERROR: pathprobe: propose/advisor requires superuser`.

Advisor mode asks a narrow question:

```text
Would any simple hypothetical indexes improve this query's plan?
```

It does not create indexes.  It does not prove that an index is globally good.
It only shows that a hypothetical index candidate helped this query in the real
planner competition.

## Advisor example

```sql
SELECT pathprobe_propose_json(
  $$ SELECT * FROM t WHERE id = 42 AND v > 90 $$,
  '{"advise": true, "advise_skip_prefix": true}'
)::jsonb;
```

Machine-readable fields:

```text
advisor.enabled               advisor ran
advisor.truncated             candidate cap was hit
advisor.atoms                 compact description of derived index atoms
advisor.baseline_cost          plan cost without advisor candidates
advisor.plan_cost              plan cost with advisor candidates
advisor.query_level_benefit    baseline_cost - plan_cost
hypo_indexes[].ddl             copy-pasteable CREATE INDEX text
hypo_indexes[].in_plan         fake index was used in the final plan
hypo_indexes[].accepted        accepted event count for this fake index label
hypo_indexes[].rejected        rejected event count for this fake index label
hypo_indexes[].survived        survived event count for this fake index label
```

A tool should treat `hypo_indexes[].in_plan = true` as stronger evidence than
"candidate was generated".  Generated-but-not-used indexes are usually noise.

## Supported advisor shapes

The advisor derives index atoms from planner-visible restrictions.  In current
scope it handles:

```text
column = constant
column range constant
column IN (...)
column = ANY(array)
column IS NULL
boolean clause forms that core PostgreSQL can match to boolean indexes
simple expression arguments
```

Current v1 skips:

```text
OR branches
join quals
IS NOT NULL
unsupported BooleanTest forms
```

Skipped does not mean impossible.  It means this advisor pass intentionally did
not model that shape.

## Existing index skip

With:

```json
{"advise_skip_prefix": true}
```

advisor suppresses candidates whose key list is already covered as a prefix of
an existing plain btree index.  This is a noise-reduction heuristic, not a full
index design rule.

## Hypothetical indexes by JSON

```sql
SELECT pathprobe_propose_json(
  $$ SELECT * FROM t WHERE v = 42 $$,
  '{"hypo_indexes": [
     {"relname": "t", "name": "hx_v", "columns": ["v"]}
   ]}'
)::jsonb;
```

The JSON form is intentionally small: btree, plain columns.

## Hypothetical indexes by SQL

```sql
SELECT pathprobe_propose_json(
  $$ SELECT v FROM t WHERE id = 42 $$,
  '{"hypo_sql": [
     "CREATE INDEX hx_id_inc ON t (id) INCLUDE (v)"
   ]}'
)::jsonb;
```

The SQL form uses PostgreSQL parsing and transformation, but does not execute
the `CREATE INDEX`.  It is suitable for expressions, partial predicates, INCLUDE,
ordering, opclasses, and non-btree AMs where supported by the code path.

## Practical reading rule

```text
advisor generated candidate  weak evidence
candidate accepted/rejected   planner saw it
candidate in_plan=true        strongest local evidence for this query
large query_level_benefit     useful signal, not an automatic DDL decision
```

## Non-btree hypothetical indexes

Any index AM with a registered routine works in the `hypo_sql` form
(gin/gist/brin/spgist verified): costs come from the AM's own estimator
(the core guards `hypothetical` in gin/brin and invents their statistics;
gist/spgist cost generically).  The size estimate stays btree-shaped and
is printed as `physics: modeled`.  Non-btree hypothetical indexes never
claim index-only scans (amcanreturn needs a real relation).

```sql
SELECT pathprobe_propose($$ SELECT id FROM tj WHERE j @> '{"n": 42}' $$,
  '{"hypo_sql":["CREATE INDEX hg ON tj USING gin (j)"]}');
```

## Parameterized queries

Queries may contain `$1..$n`: planning is generic (types inferred, no
values bound), so selectivities are the planner's generic-plan defaults -
honest, but not value-bound.  This matches what an agent typically has:
query text without concrete values.
