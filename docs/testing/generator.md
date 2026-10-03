# Generated combinatorial tests

`pathprobe_combo.sql` is the right direction: it enumerates candidate dimensions,
runs `pathprobe_propose_json()`, and checks invariants instead of costs.

The next step is to generate that SQL from an explicit matrix.

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

The generator writes:

```text
sql/pathprobe_combo_generated.sql
expected/pathprobe_combo_generated.out
docs/testing/generated-matrix.md
```

The SQL output should print counters only:

```text
NOTICE:  combinatorial generated: 128 cases, 0 failures
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
survived <= accepted
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
