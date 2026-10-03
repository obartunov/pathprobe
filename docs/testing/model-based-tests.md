# pathprobe model-based tests

## Purpose

Two model-based test layers exercise pathprobe's propose/advisor behaviour on
top of the `sql/*.sql` pg_regress smoke tests (which are retained):

- security: the ACL/RLS/view/spec authorization boundary.
- correctness: the path/hypo/advisor lifecycle reported in the JSON output.

The model is the source of truth. `t/PathprobeModel.pm` declares the
dimensions, fixtures, explicit cases, oracle, and generators. The TAP tests
`t/010_pathprobe_model_security.pl` and `t/011_pathprobe_model_correctness.pl`
consume it. There is no Python dependency.

## Requirements

- Perl with `PostgreSQL::Test::*` (from the PostgreSQL source tree) and `IPC::Run`.
- The `pathprobe` extension installed in the tree under test.

Each test brings up its own node with `PostgreSQL::Test::Cluster`, installs the
extension and fixtures (schema `ppm`, `ppm_hidden`, roles `ppm_*`), and runs the
model cases as ordinary TAP assertions. `JSON::PP` and `MIME::Base64` (both
core) are used to build specs and decode results.

## Running the tests

The tests are not wired into the Makefile or meson; run them with `prove`:

    prove t/010_pathprobe_model_security.pl t/011_pathprobe_model_correctness.pl

To run standalone against an installed cluster, set `PP_INSTALL_PATH` to the
install prefix and the usual `PostgreSQL::Test` environment:

    export PERL5LIB=/path/to/postgres/src/test/perl
    export PG_REGRESS=/path/to/postgres/src/test/regress/pg_regress
    export PATH=/path/to/install/bin:$PATH
    export PP_INSTALL_PATH=/path/to/install
    prove t/010_pathprobe_model_security.pl t/011_pathprobe_model_correctness.pl

`PP_MODE` selects the mode (default `smoke`; any other value is an error);
`PP_SEED` sets the fuzz seed
(default `42`).

## Test modes

- `smoke` (default): 25 explicit cases (15 security, 10 correctness) covering
  the P0 invariants and prior regressions.
- `pairwise`: all-pairs over the security dimensions plus the correctness
  parallelism/policy matrix.
- `targeted`: explicit 3-wise coverage of the risky interaction groups.
- `fuzz` (`PP_SEED=N`): a seeded sample of the pairwise space; not part of the
  deterministic coverage matrix.

Case and skip totals are in `model-coverage.md`.

## Case generation

Generation is bounded; it is not a full Cartesian product. It uses equivalence
classes over roles, query shapes, and spec shapes; explicit constraints that
reject redundant or structurally invalid combinations; pairwise coverage; and
targeted 3-wise groups. Every rejected combination is recorded with a reason,
listed by `tools/pathprobe_model_report.pl skips` and summarised by its
`matrix` action.

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
a `coverage` object is present; per-candidate `survived <= accepted` and
`displaced <= accepted` where acceptance is covered; `target_not_found` and
`not_examined` imply zero event counts; `fate` within `{target_not_found,
not_examined, unknown, rejected, unknown_after_accept, survived_at_point,
survived, displaced, survived_upper}` (`inconsistent` is public but fails the
check); hypo `in_plan` implies `injected` and `survived <=
accepted`; hypo `fate` within `{target_not_found, not_examined, not_generated,
rejected, displaced, survived_at_point, survived, unknown, inconsistent}` and not
`inconsistent` (a separate failing check), zero counts for `target_not_found`,
`not_examined` and `not_generated`, not injected implies `target_not_found` or
`not_examined`, and `index_build` present exactly when `coverage.index_build`;
`advisor.enabled` implies `baseline_cost` and `plan_cost` with
`query_level_benefit == baseline_cost - plan_cost`; and, when `max_events` is
set, `events_truncated` reflects the cap and the emitted event count does not
exceed it.

Cases declare `allowed_fates` (a set) only where the fate is well-defined; exact
costs are not asserted. An injected candidate is comparable to the base scan
only when their `parallel_safe` flags match: cost-based cases align
`parallel_safe` with the pinned base scan so cost decides, while
`winner_forbidden` relies on the default mismatch and therefore uses an allowed
fate set rather than a single fate.

## Offline model report

`tools/pathprobe_model_report.pl` reads the model without a database:

    perl tools/pathprobe_model_report.pl --mode MODE matrix     # coverage matrix (default)
    perl tools/pathprobe_model_report.pl --mode MODE list       # cases
    perl tools/pathprobe_model_report.pl --mode MODE skips      # rejected combinations
    perl tools/pathprobe_model_report.pl --mode MODE emit-sql   # per-case SQL

The `emit-sql` output is for inspection or export only. No generated SQL is
committed; the model cases remain the source of truth.

## Maintenance notes

- Regenerate the coverage matrix after changing the model:
  `perl tools/pathprobe_model_report.pl --mode MODE matrix` (see
  `model-coverage.md`).
- `pathprobe_json` is not covered by these propose/advisor model tests; it has a
  separate observation-path ACL review.
