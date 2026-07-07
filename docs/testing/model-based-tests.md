# pathprobe model-based tests

## Purpose

Two model-based test layers exercise pathprobe's propose/advisor behaviour on
top of the `sql/*.sql` pg_regress smoke tests (which are retained):

- security: the ACL/RLS/view/spec authorization boundary.
- correctness: the path/hypo/advisor lifecycle reported in the JSON output.

The model definitions are the source of truth. `tools/pathprobe_model_cases.py`
declares the dimensions, fixtures, explicit cases, oracle, and generators;
`tools/pathprobe_model_runner.py` installs fixtures, runs each case, and checks
the declared expectations.

## Requirements

- `python3` and `psql` (no third-party Python packages).
- A running cluster with the `pathprobe` extension available.
- `prove` for the optional TAP wrappers.

Connection uses the standard environment plus `PP_PSQL`:
`PP_PSQL`, `PGHOST`, `PGPORT`, `PGUSER`, `PGDATABASE`.

## Running the tests

    PP_PSQL=/path/to/psql PGHOST=/tmp PGPORT=5432 PGUSER=postgres \
      PGDATABASE=postgres python3 tools/pathprobe_model_runner.py --mode smoke

The runner creates its own fixtures (schema `ppm`, `ppm_hidden`, roles `ppm_*`)
and drops them on exit. `--no-setup` reuses existing fixtures; `--keep` leaves
them installed. `--only security|correctness` restricts the run. Exit status is
non-zero if any case fails.

## Test modes

- `smoke` (default): 25 explicit cases (15 security, 10 correctness) covering
  the P0 invariants and prior regressions.
- `pairwise`: all-pairs over the security dimensions plus the correctness
  parallelism/policy matrix.
- `targeted`: explicit 3-wise coverage of the risky interaction groups.
- `fuzz --seed N`: a seeded sample of the pairwise space; not part of the
  deterministic coverage matrix.

Case and skip totals are in `model-coverage.md`.

## Case generation

Generation is bounded; it is not a full Cartesian product. It uses equivalence
classes over roles, query shapes, and spec shapes; explicit constraints that
reject redundant or structurally invalid combinations; pairwise coverage; and
targeted 3-wise groups. Every rejected combination is recorded with a reason,
listed by `--list-skips` and summarised by `--emit-matrix`.

Constraints include: `none` spec is not a probe call; structured hypo on a view
or on an absent column is a spec error, not a security outcome; superuser bypass
is uniform (representatives only); multi-statement/DML are rejected at the
raw-parse guard; RLS fail-closed precedes the hypo column check; malformed hints
are covered by allowed/forbidden `top_select` representatives.

Targeted 3-wise groups: role × query shape × relation privilege;
role × query allowedness × malformed hints; role × structured hypo × column
privilege; advisor × hypo × column privilege; view/rewrite × checkAsUser ×
non-superuser; and, for correctness, winner_forbidden × parallelism ×
path competition.

## Security checks

Each security case declares one or more expectations:

- `OK`: the call returns valid JSON.
- `DENIED`: the call fails with a permission/feature error (SQLSTATE `42501` or
  `0A000`). A spec-validity error (e.g. `22023`) is not accepted as `DENIED`;
  this distinguishes authorization from spec parsing.
- `NO_LEAK`: a forbidden hidden name (one the caller did not supply in the query
  or spec) must not appear in the output or error text. Matching is
  identifier-token aware, so `private` does not match inside `v_private`; names
  the caller supplied are excluded before checking.
- `NO_GUC_LEAK`: the session's planner `enable_*` GUCs are unchanged across a
  denied or malformed hinted probe (captured before/after in one session).

## Correctness checks

Structural invariants on the returned JSON: `events[].seq` strictly increasing;
per-candidate `survived <= accepted`; `not_injected` implies zero event counts;
`fate` within `{not_injected, survived, survived_upper, accepted_not_survived,
rejected, unknown}`; hypo `in_plan` implies `injected` and `survived <=
accepted`; `advisor.enabled` implies `baseline_cost` and `plan_cost` with
`query_level_benefit == baseline_cost - plan_cost`; and, when `max_events` is
set, `events_truncated` reflects the cap and the emitted event count does not
exceed it.

Cases declare `allowed_fates` (a set) only where the fate is well-defined; exact
costs are not asserted. An injected candidate is comparable to the base scan
only when their `parallel_safe` flags match: cost-based cases align
`parallel_safe` with the pinned base scan so cost decides, while
`winner_forbidden` relies on the default mismatch and therefore uses an allowed
fate set rather than a single fate.

## TAP integration

`t/010_pathprobe_model_security.pl` and `t/011_pathprobe_model_correctness.pl`
exec the runner with `--format tap` against an ambient cluster (`PG*` env,
`PP_PSQL`; `PP_MODE` selects the mode, default `smoke`). They are runnable with
`prove` and are not wired into the extension Makefile, so `make installcheck`
(the `sql/*.sql` slices) is unaffected.

    PP_PSQL=/path/to/psql PGHOST=/tmp PGPORT=5432 PGUSER=postgres \
      prove t/010_pathprobe_model_security.pl t/011_pathprobe_model_correctness.pl

## SQL debug output

`--emit-sql` prints the per-case SQL for inspection or export only. No generated
SQL is committed; the model cases remain the source of truth.

## Maintenance notes

- Regenerate the coverage matrix after changing the model:
  `python3 tools/pathprobe_model_runner.py --emit-matrix` (see `model-coverage.md`).
- `pathprobe_json` is not covered by these propose/advisor model tests; it has a
  separate observation-path ACL review.
