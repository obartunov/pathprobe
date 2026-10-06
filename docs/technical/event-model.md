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
kept_reason     SURVIVED of a real path kept without being the cheapest
                total: startup or order?
path_id         identity of the path within this result (null without a Path)
displacer_path_id, displacer_seq, other_pathtype
                DISPLACED only: the path that displaced this one, its
                ACCEPTED event and its type
remover_path_id, remover_seq, remover_pathtype
                REMOVED only: the path that removed this one and was then
                rejected, its REJECTED event and its type
dominator_path_id, dominator_seq, dominator_pathtype
                REJECTED only: the old path add_path() stopped at as
                dominating this one, its ACCEPTED event into the same rel
                and its type
cost_startup, cost_total
                path costs where meaningful
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
displaced previously accepted path was pushed out by a path add_path()
          accepted
removed   previously accepted path was pushed out by a path add_path()
          then rejected
```

`add_path()` can remove old paths that the new path dominates and then find
a later old path that dominates the new one.  The new path is rejected; the
old paths it removed get `removed`, right after its `rejected` event.  Like
`displaced`, `removed` is terminal for the path in that rel.

## Absent events

Four different reasons can leave a target without events, and the fates keep
them apart:

```text
target_not_found  spec/identity failure: the spec names no relation of the
                  query
not_examined      the target is known, but the decision point was not reached
                  (relation removed before planning; partial index predicate
                  not implied, so build_index_paths() is never called)
not_generated     hypothetical indexes only: build_index_paths() was called
                  and built no path
unknown           missing observation: the point that would decide is not
                  covered in this run, or the outcome is not attributed
```

`not_examined` and `not_generated` are claimed only from a positive
observation (the relation was not planned; no build call happened while the
index-build point is covered).  When the point is not covered the result is
`unknown`, for example with `coverage.index_build = false`, which makes
`not_generated` unobservable.

## Candidate fate

A missing event is not lifecycle evidence.  The fate of a labeled proposed
candidate is derived only from the events that were observed and from the
run's `coverage` (see `json-format.md`): when the decision point that would
report an outcome is not covered, the result is one of the unknown states
rather than an outcome inferred from the absence of an event.

```text
target_not_found      no relation with the target name in the query
not_examined          target in the query, but its rel never reached the
                      injection point (e.g. removed by join removal)
unknown               injected, no outcome observed
rejected              rejected by add_path()
unknown_after_accept  accepted, later outcome not observed
survived_at_point     in the pathlist right after its own add_path();
                      displacement is not covered in this run
survived              as above, and no displacement or removal observed
                      while both are covered
displaced             accepted, later pushed out by a better path
removed               accepted, later pushed out by a path that add_path()
                      then rejected
survived_upper        the candidate path itself was accepted into an upper rel
inconsistent          the events contradict a covered decision point
```

A SURVIVED event is positive evidence of acceptance even when acceptance is
not covered.  `inconsistent` means only a contradiction against a covered
point, for example SURVIVED without ACCEPTED while acceptance is covered; it
indicates a probe bug or an unmodeled planner action.  Tools should treat
`unknown`, `unknown_after_accept` and `inconsistent` as non-green outcomes.

## Hypothetical index fate

A hypothetical index yields several paths, so its fate aggregates the labeled
events of all of them at its relation, after the build_index_paths()
decision reported by the core index path hook.  The hook records only
pathprobe's hypothetical indexes (matched by their IndexOptInfo); real
indexes are not logged.  In JSON the state is `fate`, and the parenthesized
detail is `fate_reason` (for `not_examined` and `unknown`) or
`index_build.not_generated` (for `not_generated`).

```text
target_not_found      no relation for it in the query
not_examined          build_index_paths() never called for it: relation not
                      planned, or partial predicate not implied
not_generated         called, no path built (reasons in index_build)
rejected              every observed add_path() rejected its paths
displaced             paths were accepted, all later displaced or removed,
                      at least one displaced
removed               paths were accepted, all later removed
survived              some accepted path was neither displaced nor removed
survived_at_point     in the pathlist right after injection; acceptance or
                      displacement not covered
unknown               the deciding point is not covered, or no generated
                      path carries the label (only inside BitmapAnd/Or)
inconsistent          the events contradict a covered decision point
```

Without index-build coverage a hypo with no labeled event is `unknown`,
never `not_generated`; an unproven partial predicate is then `unknown` too,
because an OR arm can still examine such an index.  A call is made for every
index create_index_paths() does not skip, so under coverage zero calls with
an implied predicate, or labeled events without a generating call, is
`inconsistent`.  `inconsistent` is part of the public alphabet but, as for
candidates, never a green outcome; the model and combo tests fail on it.

"At its relation" means the event's planner relids equal those of the rel
the index was injected into; a path carried unchanged into an upper rel is
counted in hypo_indexes[] but does not move the fate.

## Invariants

Core invariants:

```text
seq is strictly increasing in emitted events
survived and displaced counts cannot exceed the accepted count for the same
  labeled candidate, where acceptance is covered
target_not_found and not_examined candidates have zero
  accepted/rejected/survived/displaced counts
in_plan hypothetical index implies injected=true
hypothetical target_not_found / not_examined / not_generated have zero
  accepted/rejected/survived/displaced counts
hypothetical fate is never inconsistent in a full-hook build
a hypothetical index that was not injected is target_not_found or not_examined
truncation must be reported in limits
summary rows are derived from events, not collected separately
```

Generated tests should assert these invariants instead of exact costs.
