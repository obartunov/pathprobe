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
public repository:            OK
trusted / superuser use:      OK
untrusted propose/advisor use: blocked until a table-owner/ACL model exists
```

`pathprobe_propose` / `pathprobe_propose_json` (and therefore the advisor
and hypothetical indexes) are **superuser only** in this release.  This is a
conservative safety default, not the design goal (see Roadmap).  That
gate is what makes public, trusted use safe today: only superusers can
reach the propose/advisor surface.

The remaining blocker is *widening* that surface to untrusted users.
"Planning-only" is not harmless - an agent could mass-probe foreign
tables through the planner - so exposing propose/advisor to non-superusers
is blocked until an explicit table-owner / ACL model exists.  Observation
(`pathprobe`, `pathprobe_json`) has no such restriction.


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

### P0 agent usability: ACL-aware propose/advisor

Replace the blanket superuser-only gate with ACL-aware checks so that
non-superuser roles can use pathprobe for queries and relations they are
already allowed to inspect.  Planning-only is still security-sensitive: the
query is not executed, but planner output can reveal relation and index
names, row estimates, selectivity, path competition, and hypothetical index
usefulness.  That is acceptable for tables the caller may `SELECT`; it is not
acceptable for tables the caller cannot access.

Proposed first policy:

```text
if superuser:
    allow current behavior
else:
    allow only SELECT queries
    require SELECT privilege on every referenced relation
    respect normal caller privileges and RLS / security quals
    allow structured hypo_indexes only for referenced readable relations
    disallow hypo_sql unless superuser or table owner
    advisor may suggest indexes but must not create anything
```

Tests required before relaxing the gate:

```text
1. role with SELECT on public_t       -> pathprobe_propose_json OK
2. role without SELECT on private_t   -> permission denied
3. role with SELECT but hypo_sql      -> permission denied (non-owner)
4. superuser                          -> current behavior OK
5. RLS table                          -> path generated under caller context
```

Superuser-only is the safe initial rule, not the target model.

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
