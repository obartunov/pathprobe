# Generated combinatorial tests

`sql/pathprobe_combo.sql` enumerates candidate dimensions in a hand-written
PL/pgSQL loop, runs `pathprobe_propose_json()`, and checks invariants instead of
costs.  `tools/gen_pathprobe_tests.py` generates the same kind of SQL from an
explicit matrix; its smoke output is the `pathprobe_combo_generated` slice.

The SQL-level invariants in both must stay in step with the Perl TAP model
oracle (`t/PathprobeModel.pm`, see `model-based-tests.md`).

## Why generate

A hand-written nested loop is good for one compact case.  A generator gives:

```text
named cases
smoke / pairwise / full modes
coverage report
future dimensions without rewriting PL/pgSQL
stable expected output
```

## Dimensions

Initial dimensions:

```text
rel        combo / no_such_rel
forbid     winner_forbidden true / false
cost       cheap / expensive
ordered    pathkeys claim true / false
hypoform   none / hypo_indexes JSON / hypo_sql / advise
cap        normal / small max_events
```

Future dimensions:

```text
multi_candidate
hints
query_shape
index_state
build_capability
permission_mode
```

## Output shape

```sh
python3 tools/gen_pathprobe_tests.py --mode smoke \
  --sql-out sql/pathprobe_combo_generated.sql \
  --expected-out expected/pathprobe_combo_generated.out \
  --matrix-out docs/testing/generated-matrix-smoke.md
```

`--mode` is `smoke`, `pairwise` or `full`; `--list-cases` prints case names.
The committed SQL differs from the generator output only in its header
(`CREATE EXTENSION` instead of `LOAD`).  The SQL prints counters only:

```text
NOTICE:  combinatorial generated smoke: 8 cases, 0 failures
```

The expected file should remain stable under cost drift.

## Assertions

The generated test should assert:

```text
candidates[] exists and has expected length
candidate fate belongs to allowed alphabet
candidate injected iff target relation is present
injected=false => target_not_found
target_not_found / not_examined => no accepted/rejected/survived/displaced events
survived <= accepted, displaced <= accepted (where acceptance is covered)
hypo in_plan => injected
hypo survived <= accepted
hypo fate belongs to the hypo alphabet, and is not inconsistent
hypo target_not_found / not_examined / not_generated => no events
hypo injected=false => target_not_found or not_examined
advisor enabled => baseline_cost and plan_cost exist
limits block exists
truncated => emitted events do not exceed cap
seq strictly increases in emitted order
```

Use `coalesce()` for nullable JSON booleans.  In PL/pgSQL, `IF NULL THEN` is not a
failure; it behaves as not true.  Therefore missing fields must be turned into
explicit false or explicit assertion errors.
