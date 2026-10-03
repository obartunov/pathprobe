# pathprobe: path-level planner observability

`pathprobe` is a diagnostic extension for the PostgreSQL planner.

```text
EXPLAIN shows the winning plan.
pathprobe shows the planner path lifecycle.
```

`EXPLAIN` prints the plan that survived.  `pathprobe` records what happened
on the way, while the planner was still comparing paths: which strategies
were excluded, which paths were skipped before they were built, rejected by
`add_path()`, accepted and later displaced, or survived.  It also lets you
inject proposed paths and hypothetical indexes into that real competition and
reports what happened to them.

Four rules run through the whole output:

```text
Injected is not generated.
Generated is not accepted.
Accepted is not survived.
Missing observation is not a planner state.
```

`pathprobe` is a measuring instrument.  Observation mode does not change the
planner search.  Propose and hypo modes run counterfactual planning inside the
probe; they do not persist objects or execute proposed paths.

## Why EXPLAIN is not enough

For a two-table join, `EXPLAIN` shows a hash join over two sequential scans.
`pathprobe` on the same query shows that merge and nested loop joins were
built, accepted into the pathlist and then displaced; that other join paths
were rejected or skipped by precheck; that a parameterized index scan
survived at the base relation but no join using it won.  With
`enable_mergejoin = off` it reports merge join as `excluded`, where `EXPLAIN`
shows only its absence.  See [example 1](docs/examples/01-explain-is-not-enough.md).

## What pathprobe observes

```text
observation   pathprobe(query), pathprobe_json(query), EXPLAIN with
              pathprobe.explain = on: real planner path events
propose       pathprobe_propose[_json](query, spec): inject a diagnostic
              CustomPath and report its fate
hypo/advisor  same functions: hypothetical indexes (hypo_indexes, hypo_sql)
              or advisor-derived ones, and the fate of each
```

Event kinds: `excluded`, `skipped`, `rejected`, `accepted`, `displaced`,
`survived`.  Every event has a sequence number, the relation, the path type
and, for proposed and hypothetical paths, a label.

## Current status

The current line is v1.3 plus the lifecycle work: candidate fates derived
only from observed events, explicit coverage, DISPLACED events from
`add_path()`, and the fate of hypothetical indexes from the
`build_index_paths()` hook.  Extension SQL level 1.1, JSON format version
1.1.  Development targets PostgreSQL 20devel (master).

## Install

```sh
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
make PG_CONFIG=/path/to/pg_config installcheck
```

```sql
CREATE EXTENSION pathprobe;
```

A loaded extension adds planner hook dispatch even when no probe is active.
Use it for targeted diagnostics, tests and preflight checks, not as tracing
for every production query; see [overhead](docs/technical/overhead.md).

## Core hooks patch and coverage

Full coverage needs a server built with
[patches/pathprobe-core-hooks.patch](patches/pathprobe-core-hooks.patch)
(hook pointers in `add_path()`, the join precheck and `build_index_paths()`;
no injection points).  See [patches/APPLYING.md](patches/APPLYING.md).
Built without the patch (and without `-DHAVE_PATHPROBE_HOOKS`), the extension
still works with reduced coverage.

Every JSON report has a `coverage` object: which planner decision points
were instrumented in this run.  Missing events are not treated as facts: the
absence of an event is evidence only for a covered point, and an uncovered
point gives an `unknown` fate.

```text
add_path_accepted, add_path_displaced  core patch and pathprobe.log_accepted
                                       (the propose functions turn it on)
add_path_rejected                      core patch and pathprobe.min_gap = 0
add_path_precheck                      core patch
index_build                            core patch with the index path hook;
                                       propose functions, hypothetical indexes
survived_snapshot, join_excluded       also without the patch
```

## Basic usage

```sql
SELECT pathprobe('SELECT * FROM t WHERE id = 42');          -- text, for people
SELECT pathprobe_json('SELECT * FROM t WHERE id = 42')::jsonb;  -- for tools

SET pathprobe.explain = on;
EXPLAIN SELECT * FROM t WHERE id = 42;   -- adds a PathProbe section
```

Tools should read the JSON (`events`, `summary`, `coverage`, `limits`), not
the text table.  See [quickstart](docs/user/quickstart.md) and
[reading output](docs/user/reading-output.md).

## Proposed paths

A proposed path is a diagnostic `CustomPath` with given costs and
properties, injected into real `add_path()` competition at a named
relation.  Each candidate gets a fate:

```text
target_not_found  not_examined  unknown  rejected  unknown_after_accept
survived_at_point  survived  displaced  survived_upper  (inconsistent)
```

`target_not_found` is a spec error (no such relation in the query);
`not_examined` means the relation is in the query but planning never reached
the injection point; `unknown` means the outcome was not observed.
`inconsistent` appears only if events contradict a covered decision point,
and is never a normal result.  With `winner_forbidden` the candidate competes
but cannot become the executable plan; a proposed path that reaches the
executor raises an error.  See [proposed paths](docs/user/proposed-paths.md).

## Hypothetical indexes and not_generated

`hypo_indexes` (columns) and `hypo_sql` (`CREATE INDEX` text, superuser) add
hypothetical indexes to the planner; `{"advise": true}` lets the advisor
derive them from the query.  `injected=true` only says the index was offered.

A hypothetical index may be injected and examined, but still generate no
IndexPath.  pathprobe reports that as `not_generated`, with the reason in
`index_build.not_generated` (`no_useful_input`, `no_leading_clause`,
`scantype_unsupported`), when index-build coverage is available.  The full
hypothetical index fate:

```text
target_not_found  not_examined  not_generated  rejected  displaced
survived  survived_at_point  unknown  (inconsistent)
```

with `fate_reason` for `not_examined` (`rel_not_planned`,
`predicate_not_implied`) and `unknown` (`index_build_not_covered`,
`predicate_not_implied`, `outcome_not_observed`).  `in_plan` says separately
whether the final plan uses the index.  Without `coverage.index_build` a
hypothetical index with no events is `unknown`, never `not_generated`.  Only
pathprobe's hypothetical indexes are recorded at `build_index_paths()`; real
indexes are not.  See [advisor and hypothetical indexes](docs/user/advisor.md).

## Examples

[docs/examples/README.md](docs/examples/README.md) has nine runnable stories,
each a psql script under `examples/` with structural checks
(`examples/run_all.sh` runs them all; superuser and the core patch needed):

```text
1  EXPLAIN is not enough
2  displaced paths
3  target_not_found vs not_examined vs unknown
4  not_generated index paths
5  hypothetical index lifecycle
6  agent preflight evidence
7  advisor candidate lifecycle
8  advisor limitations
9  advisor audit over common query patterns
```

## Testing

```text
make installcheck             pg_regress slices: observation, propose,
                              agent output, combinatorial invariants
                              (hand-written and generated), security,
                              lifecycle
t/010, t/011 (prove)          native Perl TAP model tests over
                              PostgreSQL::Test::Cluster: security boundary
                              and lifecycle correctness; smoke, pairwise,
                              targeted and seeded fuzz modes
tools/gen_pathprobe_tests.py  generator of the combinatorial SQL slice
examples/run_all.sh           the example scripts
```

Tests assert structure and invariants (fate alphabets, coverage-conditioned
counts), never exact costs.  See [model-based tests](docs/testing/model-based-tests.md).

## Security model

Observation (`pathprobe`, `pathprobe_json`) has no extra restriction.  The
propose functions, and with them the advisor and hypothetical indexes, are
full-featured for superusers and run in a restricted, ACL-aware mode for
everyone else: a single `SELECT`; every query level authorized under the
caller's own privileges; relations with row-level security and views or
rules that would expose a base relation under another role refused (fail
closed); `hypo_sql` denied; structured hypothetical indexes btree-only on
relations and columns the caller may read.  Planner output reveals names,
estimates and selectivity, which is why the mode is conservative.  See
[safety and limits](docs/technical/safety-and-limits.md).

## Agent preflight boundary

pathprobe provides structured planner evidence for preflight decisions: for
example coverage gaps, truncation, relations where only a sequential scan
survived, excluded strategies, and the fate of a candidate index.  The policy
decision is outside pathprobe.  See [agent preflight](docs/user/agent-gate.md)
and [example 6](docs/examples/06-agent-preflight.md).

## Documentation map

```text
docs/user/quickstart.md           first commands
docs/user/reading-output.md       event words, sources, summary
docs/user/proposed-paths.md       candidate injection and fate
docs/user/advisor.md              advisor and hypothetical indexes
docs/user/agent-gate.md           evidence for preflight policies
docs/user/comparison.md           EXPLAIN, HypoPG, pg_plan_advice and others
docs/examples/                    the nine runnable stories
docs/technical/event-model.md     event kinds, fate alphabets, invariants
docs/technical/json-format.md     machine-readable output, coverage
docs/technical/architecture.md    layers
docs/technical/implementation.md  code map
docs/technical/output-format-audit.md  readability audit of the output
docs/technical/advisor-audit.md   what the advisor generates, skips and gets wrong
docs/technical/safety-and-limits.md, overhead.md
docs/testing/                     model-based tests, generator, test matrix
```

## Limitations

- Reduced builds see less: without the core patch only survivor snapshots
  and join exclusions are observed; without the index path hook there is no
  `not_generated`.
- Hypothetical index paths that end up only inside a BitmapAnd/BitmapOr
  carry no label; their outcome is `unknown(outcome_not_observed)`.
- When `apply_scanjoin_target_to_paths()` rebuilds a pathlist, the paths it
  drops produce no DISPLACED event.
- The JSON event does not name the path that displaced another; it is the
  ACCEPTED event just before the DISPLACED ones.
- The advisor is a local candidate generator with known defects (OR
  clauses, duplicate expression indexes, hypothetical btree costing); see
  [advisor audit](docs/technical/advisor-audit.md).
- `excluded` reports that a join family was blocked, not who blocked it
  (`enable_*`, pg_plan_advice or another extension).
- Not supported for non-superusers yet: probing relations with row-level
  security under the caller's policies, `hypo_sql` for table owners, probing
  through views without exposing base relations, expression or partial
  hypothetical indexes.

## License

PostgreSQL License; see [LICENSE](LICENSE).
