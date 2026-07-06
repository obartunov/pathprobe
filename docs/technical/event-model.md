# Event model

The event model is the core of `pathprobe`.

## Event identity

Every event has a monotonic sequence number.

```text
seq increases within one probe run
events are append-only
summary is derived later
```

A tool may use `seq` to reconstruct local order.  It must not assume that event
order alone identifies a candidate; proposed and hypothetical paths need labels.

## Event fields

Important event fields:

```text
seq             causal order inside one run
src             source adapter: pc, p1, p0, j2, wp
rel             relids set
relname         human relation label where available
pathtype        PostgreSQL path type name
kind            excluded/skipped/rejected/accepted/displaced/survived
origin          real/proposed
label           candidate or hypothetical path identity
cost_total      path total cost where meaningful
gap_cost        difference from local competitor where meaningful
skip_reason     precheck reason where available
disabled_nodes  planner disabled-nodes marker
```

`label` is required for proposed candidates and hypothetical indexes.  Without a
label, a machine reader cannot reliably connect events to a candidate.

## Lifecycle alphabet

```text
excluded  strategy blocked before generation
skipped   candidate filtered before a Path object existed
rejected  materialized Path lost in add_path()
accepted  materialized Path entered the frontier
survived  path was present in an observed frontier
displaced previously accepted path was pushed out
```

## Candidate fate state machine

For a labeled proposed candidate:

```text
no events                  not_injected
rejected event             rejected
accepted, no survivor      accepted_not_survived
survived at base frontier  survived
survived at upper rel      survived_upper
bad transition             inconsistent
insufficient evidence      unknown
```

The exact C code may refine this, but the public model should remain finite and
explicit.  Tools should treat unknown/inconsistent as non-green outcomes.

## Invariants

Core invariants:

```text
seq is strictly increasing in emitted events
survived count cannot exceed accepted count for the same labeled candidate
not_injected candidates have zero accepted/rejected/survived counts
in_plan hypothetical index implies injected=true
truncation must be reported in limits
summary rows are derived from events, not collected separately
```

Generated tests should assert these invariants instead of exact costs.
