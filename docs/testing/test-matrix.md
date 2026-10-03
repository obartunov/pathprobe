# Test matrix

The regression goal is not exact text or exact cost stability.  The goal is to
keep planner-observability semantics stable.

## Test classes

```text
smoke      small fast suite for make check
pairwise   generated pair coverage across dimensions
full       exhaustive product for manual/nightly runs
```

## Core invariant tests

```text
json escaping
candidate fate alphabet
injected vs target relation
not-injected consistency (target_not_found)
target_not_found / not_examined carry no events
survived <= accepted, displaced <= accepted (coverage-conditioned)
hypo in_plan -> injected
hypo fate alphabet and not-generated/not-injected consistency
advisor block completeness
limits block presence and truncation behavior
seq monotonicity
```

## Coverage table

```text
scenario                                smoke  pairwise  full
----------------------------------------------------------------
JSON escaping                             yes     yes     yes
weak proposed loser                       yes     yes     yes
cheap forbidden candidate                 yes     yes     yes
nonexistent target                        yes     yes     yes
pathkeys claim                            yes     yes     yes
hypo_indexes JSON form                    yes     yes     yes
hypo_sql CREATE INDEX form                yes     yes     yes
advisor enabled                           yes     yes     yes
small max_events truncation               yes     yes     yes
multi-candidate same relation             no      yes     yes
multi-candidate different relations       no      yes     yes
hints forbid                              no      yes     yes
existing prefix index skip                no      yes     yes
expression index candidate                no      yes     yes
partial index candidate                   no      yes     yes
non-btree hypothetical AM                 no      no      yes
permission denied                         no      yes     yes
execution boundary error                  yes     yes     yes
```

## Stable assertions

Prefer:

```text
JSON is valid
field exists
boolean semantics are consistent
fate is in allowed alphabet
benefit > 0 when designed to improve
in_plan=true when designed to be used
truncated=true when cap is intentionally small
```

Avoid:

```text
exact path cost
exact raw event count when planner versions differ
full text output comparison
assuming all sources exist in every build
```
