# pathprobe — path-level planner observability

`pathprobe` is a diagnostic extension for PostgreSQL planner work.  It shows
not only the final plan chosen by `EXPLAIN`, but also the path-level decisions
made on the way: which paths were skipped, rejected, accepted, displaced, or
survived.

The extension has three user-facing modes:

```text
ordinary probe      observe real planner path events
proposed paths      inject diagnostic candidates into real add_path() competition
advisor / hypo      test hypothetical indexes before creating anything
```

`pathprobe` is a measuring instrument.  It does not replace the optimizer and it
does not execute proposed or hypothetical paths.  Its value is evidence: the
answer comes from real PostgreSQL planner competition, not from a separate model.

## Security status

```text
public repository:                  OK
trusted / superuser use:            OK
non-superuser propose/advisor use:  OK, restricted ACL-aware mode
```

`pathprobe_propose` / `pathprobe_propose_json` (and therefore the advisor
and hypothetical indexes) run in one of two modes:

- **superuser** - full behavior.
- **non-superuser** - a restricted, ACL-aware mode.  The probe must be a
  single `SELECT` statement; every `Query` level (subqueries, CTEs, sublinks,
  set-op arms) is authorized with the caller's own privileges via the core
  permission check, so only relations and columns the caller may already
  `SELECT` are reachable.  Relations with row-level security are refused
  (fail-closed), and a view or rule that would reach a base relation under
  another role's privileges is refused (fail-closed).  `hypo_sql` is denied;
  structured hypothetical indexes are btree-only and limited to relations and
  columns the caller is authorized for.

"Planning-only" is still security-sensitive - planner output can reveal
relation/index names, row estimates, selectivity, and path competition - so
the non-superuser mode is deliberately conservative and fails closed.
Observation (`pathprobe`, `pathprobe_json`) has no such restriction.


## Install

```sh
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
make PG_CONFIG=/path/to/pg_config installcheck
```

Then, in a database:

```sql
CREATE EXTENSION pathprobe;
```

Full path coverage (probe1 accepted/rejected/displaced events) needs the
core hooks patch; stock builds work with reduced coverage.  See
[patches/APPLYING.md](patches/APPLYING.md).

pathprobe is designed so expensive work happens only when a probe is
active, but a loaded extension still adds planner hook dispatch overhead.
Use it for targeted diagnostics, tests, and agent preflight, not as
blanket tracing for every production query.  See
[docs/technical/overhead.md](docs/technical/overhead.md).

## Roadmap

### Done: ACL-aware propose/advisor (former P0)

The blanket superuser-only gate has been replaced with the ACL-aware model
described under Security status: a non-superuser may probe queries, relations,
and columns it can already `SELECT`, with every `Query` level authorized under
the caller's privileges, RLS and view/rule base exposure failing closed, and
`hypo_sql` denied.

### Deferred

```text
RLS-compatible mode          probe RLS relations under the caller's policy
owner-level hypo_sql         allow hypo_sql for a table owner, not only superuser
view-safe redaction          probe through a view without exposing its base relation
expression/predicate hypo    non-superuser hypo beyond plain btree key columns
model-based security runner  exhaustive model-driven security regression runner
```

## Where to start

For users:

```text
docs/user/quickstart.md          first commands
docs/user/reading-output.md      how to read excluded/skipped/rejected/accepted/survived
docs/user/proposed-paths.md      counterfactual path injection
docs/user/advisor.md             advisor and hypothetical indexes
docs/user/agent-gate.md          planner-aware SQL agent gate
docs/user/comparison.md          how pathprobe differs from EXPLAIN, HypoPG, etc.
```

For developers:

```text
docs/technical/architecture.md       internal layers (the model)
docs/technical/implementation.md     how the code implements the model
docs/technical/event-model.md        event semantics and invariants
docs/technical/json-format.md        machine-readable output
docs/technical/safety-and-limits.md  resource limits and agent-safe use
docs/technical/overhead.md           planning-time overhead: not-loaded / inactive / active
```

For tests:

```text
docs/testing/test-matrix.md      coverage matrix
docs/testing/generator.md        generated combinatorial tests
tools/gen_pathprobe_tests.py     smoke/pairwise/full SQL generator
```

## Short mental model

```text
PostgreSQL planner hooks
        ↓
append-only PathEventLog
        ↓
derived PathProbeReport
        ↓
text / JSON / EXPLAIN / reporter
```

The event log is the internal truth.  All output formats are views over it.

## Agent boundary

A SQL agent may use `pathprobe` before executing a query:

```text
query
  ↓
pathprobe_propose_json(..., '{"advise": true}')
  ↓
JSON evidence: cost, candidate fate, hypothetical index use, truncation flags
  ↓
policy decision
  ↓
execute / route to replica / suggest index / refuse
```

The agent should not parse human text.  It should read `candidates[]`,
`advisor`, `hypo_indexes[]`, `events[]`, `summary[]`, and `limits`.
