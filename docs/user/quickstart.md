# Quickstart

`pathprobe` answers a different question from `EXPLAIN`.

```text
EXPLAIN    what plan was chosen
pathprobe  what happened to planner paths before the choice became final
```

## Install

```sql
CREATE EXTENSION pathprobe;
```

That creates `pathprobe()`, `pathprobe_json()`, `pathprobe_propose()`, and
`pathprobe_propose_json()`.

<details>
<summary>Developer fallback (uninstalled checkout)</summary>

If you are running from a build tree without the extension installed, you
can load the library and declare the functions by hand:

```sql
LOAD 'pathprobe';

CREATE FUNCTION pathprobe(query text)
RETURNS text
AS 'pathprobe','pathprobe'
LANGUAGE C STRICT;

CREATE FUNCTION pathprobe_json(query text)
RETURNS text
AS 'pathprobe','pathprobe_json'
LANGUAGE C STRICT;

CREATE FUNCTION pathprobe_propose(query text, spec text)
RETURNS text
AS 'pathprobe','pathprobe_propose'
LANGUAGE C STRICT;

CREATE FUNCTION pathprobe_propose_json(query text, spec text)
RETURNS text
AS 'pathprobe','pathprobe_propose_json'
LANGUAGE C STRICT;
```

</details>

## First table

```sql
CREATE TABLE t (id int, v int);
INSERT INTO t SELECT g, g % 100 FROM generate_series(1, 10000) g;
CREATE INDEX t_id_idx ON t(id);
ANALYZE t;
```

## Human-readable probe

```sql
SELECT pathprobe('SELECT * FROM t WHERE id = 42');
```

Read the output as a path lifecycle log.  A path may be built and rejected, or it
may never be built at all.

## Machine-readable probe

```sql
SELECT pathprobe_json('SELECT * FROM t WHERE id = 42')::jsonb;
```

Use this form for tools and agents.  Do not parse the text table.

## EXPLAIN integration

```sql
SET pathprobe.explain = on;

EXPLAIN SELECT * FROM t WHERE id = 42;
EXPLAIN (FORMAT JSON) SELECT * FROM t WHERE id = 42;
```

`EXPLAIN` still shows the final plan.  With `pathprobe.explain = on`, it also
gets a `PathProbe` section with the path-level evidence.

## Proposed path smoke test

> **Access.**  Superusers get full behavior.  A non-superuser runs in a
> restricted ACL-aware mode: the probe must be a single `SELECT`, and only
> relations and columns the caller may already `SELECT` are reachable (every
> query level is authorized under the caller's privileges).  RLS relations, and
> views/rules that would reach a base relation under another role, fail closed;
> `hypo_sql` is denied and structured hypothetical indexes are btree-only on
> authorized relations and columns.  A privilege violation raises an error -
> that is expected, not a broken build.  See the README security status.

```sql
SELECT pathprobe_propose_json(
  $$ SELECT * FROM t WHERE id = 42 $$,
  '{
     "target": {"relname": "t"},
     "candidate": {"pathname": "demo_fast_scan", "provider": "demo"},
     "cost": {"total_cost": 1.0, "rows": 10},
     "mode": {"winner_forbidden": true}
   }'
)::jsonb;
```

This injects a diagnostic candidate into the real planner competition.  With
`winner_forbidden=true`, the candidate may be observed, but it is prevented from
becoming the executable winner.

## Advisor smoke test

```sql
SELECT pathprobe_propose_json(
  $$ SELECT * FROM t WHERE v > 90 ORDER BY id $$,
  '{"advise": true, "advise_skip_prefix": true}'
)::jsonb;
```

The advisor derives hypothetical index candidates, plans the query with them,
and reports which fake indexes were actually used by the final plan.
