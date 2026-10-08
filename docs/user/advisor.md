# Advisor and hypothetical indexes

> The advisor runs through `pathprobe_propose_json()`.  Superusers get full
> behavior; non-superusers run in the restricted ACL-aware mode (single
> `SELECT`; only relations and columns the caller may already `SELECT`; RLS and
> view/rule base exposure fail closed; `hypo_sql` denied; structured btree hypo
> only, on authorized relations and columns).  See the README security model.

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
advisor.truncated             candidate cap was hit (not the atom limit)
advisor.atom_limit            atoms kept per relation (6)
advisor.atoms_seen            distinct atoms found, summed over relations
advisor.atoms_kept            atoms used for candidates
advisor.atoms_dropped         atoms over the per-relation limit, not used
advisor.atom_truncated        atoms_dropped > 0: not every restriction was
                              considered
advisor.atoms                 compact description of derived index atoms
advisor.baseline_cost          plan cost without advisor candidates
advisor.plan_cost              plan cost with advisor candidates
advisor.query_level_benefit    baseline_cost - plan_cost
hypo_indexes[].ddl             copy-pasteable CREATE INDEX text
hypo_indexes[].in_plan         fake index was used in the final plan
hypo_indexes[].accepted        accepted event count for this fake index label
hypo_indexes[].rejected        rejected event count for this fake index label
hypo_indexes[].survived        survived event count for this fake index label
hypo_indexes[].displaced       displaced event count for this fake index label
hypo_indexes[].fate            not_examined / not_generated / rejected /
                               displaced / survived / ... (see event-model.md)
hypo_indexes[].index_build     build_index_paths() calls and outcome, or null
```

A hypothetical index goes through separate stages, and each has its own
evidence:

```text
advisor candidate generated   the advisor derived the index from the query
                              (advisor.atoms); says nothing about the planner
hypo index injected           injected=true: pathprobe offered it to the
                              relation's index list
IndexPath generated           index_build.generated > 0: build_index_paths()
                              built at least one path over it; otherwise
                              fate not_examined or not_generated
path accepted / rejected /    accepted, rejected, displaced, survived counts
  displaced / survived        and the fate derived from them
in_plan                       the final plan uses the index
```

Injected is not generated, and generated is not accepted.  `fate` and
`fate_reason` (see `docs/technical/event-model.md`) summarize the stages:

```text
not_examined(rel_not_planned | predicate_not_implied)
not_generated                 reasons in index_build.not_generated:
                              no_useful_input, no_leading_clause,
                              scantype_unsupported
rejected / displaced / survived / survived_at_point
unknown(index_build_not_covered | predicate_not_implied | outcome_not_observed)
target_not_found
```

`not_generated` needs `coverage.index_build`, which requires the core patch
with the index path hook.  Without it, a hypothetical index with no events
is `unknown(index_build_not_covered)`.  The hook records only pathprobe's
hypothetical indexes; real indexes are not logged.

A tool should treat `in_plan = true` as the strongest of these.  Generated
but not used indexes are usually noise.  See
[example 4](../examples/04-not-generated-index-path.md) and
[example 5](../examples/05-hypothetical-index-lifecycle.md).

## Supported advisor shapes

The advisor derives index atoms from planner-visible restrictions.  In current
scope it handles:

```text
column = constant
column range constant
column IN (...)
column = ANY(array)
column IS NULL
a boolean column or a function returning boolean, bare, under NOT, or with
  IS TRUE / IS FALSE
simple expression arguments
```

Current v1 skips:

```text
join quals (a constant reaching a join column through an equivalence class
  still produces an atom)
IS NOT NULL
OR restrictions (as a whole)
boolean expressions other than a column or a function (e.g. (a > b) IS TRUE)
unsupported BooleanTest forms
LIKE / pattern operators (no strategy in the default btree opfamily)
ORDER BY ... DESC or NULLS FIRST (no ordered candidate)
atoms beyond the sixth per relation (dropped, but counted:
  advisor.atoms_dropped, advisor.atom_truncated)
partial indexes (never proposed)
```

Skipped does not mean impossible.  It means this advisor pass does not model
that shape.  An `OR` restriction is skipped as a whole, also when all its
branches test one column (`ts = x OR ts = y` gets no `(ts)` candidate); it
is not split into branches and not proposed as a boolean expression index.
A boolean qual becomes an atom only when its operand is a boolean column or
a function returning boolean (`flag`, `NOT flag`, `flag IS TRUE`,
`isfinite(ts)`); other boolean expressions are skipped.  The audit found
that an OR used to become one boolean expression candidate; that was fixed
after the audit.

The advisor keeps at most six atoms per relation, equivalence-class
equalities first, then the other restrictions in planner order; the rest
are dropped without affecting the candidates.  The audit found that nothing
reported the dropped atoms.  After the audit they are counted:
`advisor.atom_truncated` is true, `advisor.atoms_dropped` says how many, and
the relation's `atoms` list ends with `(+N dropped)`.  When it is true the
candidates come from the first six atoms only, not from every restriction.

An expression candidate that an existing expression index already provides
is not proposed: the audit found that `(lower(d))` was proposed again next to
an existing `lower(d)` index, and that was fixed after the audit.  See
`docs/technical/advisor-audit.md` and
[example 9](../examples/09-advisor-audit.md).

## Existing index skip

With:

```json
{"advise_skip_prefix": true}
```

advisor suppresses candidates whose key list is already covered as a prefix of
an existing plain btree index, and expression candidates whose expression is
the leading key of an existing wider index.  Without the option only an exact
duplicate is suppressed.  Partial indexes never suppress a candidate.  This is
a noise-reduction heuristic, not a full index design rule.

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
advisor generated candidate   weak evidence
fate not_generated            the planner examined it and built no path
candidate accepted/rejected   a path over it competed
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
