# pathprobe model-based tests

This layer turns the two state machines that pathprobe already implements —
the **security boundary** and the **path lifecycle** — into explicit, checkable
test models.  It sits *on top of* the `sql/*.sql` pg_regress smoke tests, which
remain the small in-tree regression layer; it does not replace them.

The model is the source of truth.  SQL is emitted only for debugging
(`--emit-sql`); there is no generated-SQL wall committed to the tree.

Files:

- `tools/pathprobe_model_cases.py` — model definitions: dimensions, fixtures,
  explicit cases, the rule-based oracle, and the bounded generators.
- `tools/pathprobe_model_runner.py` — runner + oracle: installs fixtures, runs
  each case in one psql session, checks the declared expectations, reports.
- `t/010_pathprobe_model_security.pl`, `t/011_pathprobe_model_correctness.pl` —
  thin TAP wrappers (run against an ambient cluster; **not** wired into
  `make installcheck`, see below).

## Running

The runner connects with the standard `PG*` environment plus `PP_PSQL`:

```
PP_PSQL=.../psql PGHOST=/tmp PGPORT=5432 PGUSER=postgres PGDATABASE=postgres \
  python3 tools/pathprobe_model_runner.py --mode smoke
```

Modes: `smoke` (default), `pairwise`, `targeted`, `fuzz --seed N`.
Actions: `--list-cases`, `--list-skips`, `--emit-matrix`, `--emit-sql`,
`--format tap`, `--only security|correctness`, `--no-setup`, `--keep`.

The runner installs its own fixtures (schema `ppm`, `ppm_hidden`, roles
`ppm_*`) and drops them on teardown.

## Not a full Cartesian product

The generators deliberately avoid enumerating every dimension combination.
They use equivalence classes, explicit constraints (with reported reasons),
pairwise coverage, and targeted 3-wise coverage of known risky interactions.
Every rejected combination is recorded with a reason and can be listed with
`--list-skips` or summarised by `--emit-matrix`.

Approximate case budgets (kept small on purpose):

- smoke: 25 (15 security + 10 correctness)
- pairwise: ~51 (<= 150)
- targeted: ~67 (<= 80)

### Equivalence classes

Roles collapse to security-relevant classes: superuser, table_select,
column_select, no_select, no_schema_usage, rls_subject, view_subject (concrete
fixture roles `ppm_owner`, `ppm_agent_tbl`, `ppm_agent_col`, `ppm_agent_none`,
`ppm_agent_noschema`, `ppm_intruder`).  Query shapes collapse to authorizer
paths (top/cte/subquery/sublink/set-op/join/multi-statement/dml).  Spec shapes
collapse to security-relevant families (none/candidate/structured-hypo
allowed|forbidden|mixed/hypo_sql/advisor/hints valid|malformed).

### Constraints (skipped with reasons)

- `none` spec is not a probe call (`target.relname` required) — excluded.
- structured hypo on a view, or on a column the relation does not have —
  excluded as a spec error, not a security outcome.
- superuser bypasses ACL uniformly — the generic pairwise fan-out over
  superuser is skipped; representative superuser cases live in smoke/targeted.
- multi-statement / modifying command is rejected at the raw-parse guard before
  any spec handling — one representative kept.
- RLS fail-closed precedes the hypo column check — RLS × hypo skipped.
- malformed hints are covered by allowed/forbidden `top_select` representatives.

### Targeted 3-wise groups

`role×query×relation_privilege` (CTE/subquery/sublink/set-op auth leaks),
`role×query_allowedness×malformed_hints` (hints-before-auth),
`role×structured_hypo×column_privilege` (forbidden-column hypo leaks),
`advisor×hypo×column_privilege` (advisor must not surface a forbidden column),
`view/rewrite×checkAsUser×non_superuser` (view/rule base exposure), and — on the
correctness side — `winner_forbidden×parallelism×path_competition`.

## Security oracle

Each security case declares one or more of: `OK`, `DENIED`, `NO_LEAK`,
`NO_GUC_LEAK`.

- `OK` — call returns valid JSON.
- `DENIED` — call fails with a permission/feature error (SQLSTATE `42501` or
  `0A000`).  A spec-validity error (e.g. `22023` "hints must be an object") is
  **not** accepted as `DENIED`; that distinction is what proves authorization
  runs before hint processing.
- `NO_LEAK` — a forbidden **hidden** name (one the caller did not itself supply
  in the query or spec) must not appear in the output or the error text.
  Matching is identifier-token aware: `private` does not match inside
  `v_private`, while `ppm.private` matches as its own token.  Names the caller
  supplied are stripped before checking, because echoing a name the caller
  already knows is not a leak.
- `NO_GUC_LEAK` — a denied/malformed hinted probe must leave the session's
  planner `enable_*` GUCs unchanged (captured before/after in one session).

## Correctness oracle

Every correctness case checks structural invariants on the returned JSON:
`events[].seq` strictly increasing; per-candidate `survived <= accepted`;
`not_injected` implies zero event counts; `fate` within the alphabet
(`not_injected`, `survived`, `survived_upper`, `accepted_not_survived`,
`rejected`, `unknown`); hypo `in_plan` implies `injected` and
`survived <= accepted`; `advisor.enabled` implies `baseline_cost` and
`plan_cost` with `query_level_benefit == baseline_cost - plan_cost`; and, when a
`max_events` cap is set, `events_truncated` reflects the cap and the emitted
event count does not exceed it.

Cases declare `allowed_fates` (a set, not a single fate) only where the fate is
well-defined.  Exact costs are never asserted.

### The parallelism / parallel_safe lesson

An injected candidate is only comparable to the base scan when their
`parallel_safe` flags match.  With parallelism pinned off, the base scan is not
parallel-safe; a candidate left at the default `parallel_safe=true` therefore
never dominates or is dominated on cost — it simply survives (`survived_upper`)
regardless of its cost.  Consequently:

- To test **cost-based** acceptance/rejection deterministically, the model
  aligns the candidate's `parallel_safe` with the pinned base scan (`false`), so
  cost is the deciding axis.
- `winner_forbidden` survival relies on the opposite: the default `parallel_safe`
  mismatch is what keeps the forbidden winner alive as a non-winner. Under
  pinned parallelism that yields `survived_upper`; with `parallel_safe` aligned
  and cost competition it can become `rejected`.  These environment-dependent
  outcomes are expressed as allowed fate sets, never a single hard-coded fate.

## Verified behaviours behind the oracle

The oracle branches were grounded against the running extension, not assumed:
non-superuser probes over `private`, in a CTE, subquery, EXISTS sublink, or
UNION branch are all `DENIED`; any view is fail-closed for a non-superuser even
when the caller can read the base (`checkAsUser` mismatch), and the base name is
not named in the error; RLS relations are fail-closed; a structured hypo on a
forbidden column is `DENIED` (SQLSTATE `42501`) and names only the
caller-supplied column; `hypo_sql`, multi-statement, and modifying commands are
denied for non-superusers; superuser bypasses all of these.

## TAP integration

The runner already emits TAP with `--format tap`.  The `t/010` and `t/011`
wrappers exec the runner against an **ambient** cluster (via `PG*`) and are
runnable with `prove`.  They are intentionally **not** added to the extension
`Makefile`'s test wiring in this first pass, so `make installcheck` (the six
`sql/*.sql` slices) is unaffected.  Wiring them into a dedicated TAP target is a
small follow-up if desired.

## Out of scope for this layer (flagged)

`pathprobe_json` (the observation function) is planner-only.  The planner does
not enforce table-level `SELECT`; only schema `USAGE` is required to plan.  A
non-superuser with schema `USAGE` but no `SELECT` on a table can therefore call
`pathprobe_json` and obtain planner estimates for it (both `pathprobe_json` and
`pathprobe_propose_json` have default `PUBLIC EXECUTE`).  This is the
observation path, distinct from the propose/advisor boundary that this model
exercises, and is left as an open question rather than being asserted here.
