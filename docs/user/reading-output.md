# Reading pathprobe output

`pathprobe` is path-centric, not plan-centric.  The unit of observation is a
planner path event.

## Event words

```text
excluded  a join strategy was blocked before a path was generated
skipped   a candidate was filtered before a Path object was built
rejected  a Path was built, entered add_path(), and lost
accepted  a Path entered the frontier
survived  a Path is still present in the observed frontier
displaced a previously accepted Path was pushed out by a later competitor
          that add_path() accepted
removed   a previously accepted Path was pushed out by a later competitor
          that add_path() then rejected
```

These are not synonyms.  They name different stages of path elimination.
Accepted is not survived: an accepted path can be displaced later.

## Example

Summary rows of a two-table join (from
[example 1](../examples/01-explain-is-not-enough.md)):

```text
  rel           strategy             excl  skip  rej  acc  surv  best_cost
  {1,2}         MergeJoin               0     3    1    2     0           -
  {1,2}         NestLoop                0    11    4    1     0           -
  {1,2}         HashJoin                0     1    0    1     2      380.47
```

A plain reading:

```text
HashJoin survived.
MergeJoin was built and accepted twice, and is not among the survivors:
  both were displaced (the events show DISPLACED); one more was rejected,
  three were skipped by precheck before a Path existed.
NestLoop: accepted once and displaced, rejected four times, skipped eleven.
```

This is different from saying "PostgreSQL did not consider a merge join".
A `rejected` or `displaced` event means the path was built.  The summary has
no displaced column; count DISPLACED events in `events[]`.

## Coverage

An absent event is evidence only where the run covers that decision point;
the JSON `coverage` object says which ones are covered.  `accepted` and
`displaced` events are recorded only with `pathprobe.log_accepted = on`
(the propose functions turn it on), so in a plain `pathprobe()` run without
it, "no accepted event" says nothing.  See
[json-format](../technical/json-format.md#coverage).

## Path identity

In JSON every event that concerns a Path has a `path_id`; the same path keeps
it across its events (accepted, survived in a snapshot, displaced).  A
DISPLACED event names its displacer (`displacer_path_id`, `displacer_seq`,
`other_pathtype`).  A REMOVED event marks a path that a new path pushed out
of the pathlist although `add_path()` then rejected that new path; it names
the remover (`remover_path_id`, `remover_seq`, `remover_pathtype`) and, like
DISPLACED, ends the path's life in that rel: a candidate with it has the
fate `removed`, not `survived`.  A REJECTED event names the path that dominated it
(`dominator_path_id`, `dominator_seq`, `dominator_pathtype`): the first path
in the pathlist that `add_path()` found to be at least as good on cost,
ordering, parameterization, rows and parallel safety.  The top-level
`paths[]` array gives each id its type and relation, and the child edges of
join paths (`outer_path_id`, `inner_path_id`), single-input wrappers
(`subpath_id`) and paths with a list of inputs, such as Append or BitmapOr
(`child_path_ids`).  `seq` is event order, not identity.  With the core
patch, a path id is not reused within one result when the planner frees a
path and allocates another at the same address (GEQO excepted).  See
[json-format](../technical/json-format.md#path-identity-and-graph-edges).

## Sources

`src` identifies the instrumentation source:

```text
pc  precheck stage, before a materialized Path exists
p1  add_path() stage, after a Path was built
p0  base-relation frontier snapshot
j2  join-level pre-generation / pair-local frontier observation
wp  proposed candidate (CustomScan) event
```

Hypothetical index paths keep their source (`p1`, or the frontier check after
injection) and carry a `hypo:<name>` label.

The same path strategy may disappear at different sources.  That is the point of
the tool.

## Base frontier vs join frontier

`p0 survived` and `j2 survived` do not mean the same thing.

```text
p0 survived  final base relation frontier snapshot
j2 survived  pair-local join frontier snapshot for one outer × inner pair
```

Multiple `j2 survived` events are normal.  They describe join search history.

## Kept survivors

A survivor may not be the cheapest total path.  PostgreSQL can keep additional
paths for startup cost or order.

`pathprobe` annotates such cases:

```text
kept: startup
kept: order?
```

`order?` means the path carries useful ordering information.  It does not prove
that a parent node eventually used that order.

## Summary rows

The summary is a derived view over events.  It shows search shape.

```text
raw events  explain individual losses
summary     explains the elimination profile
```

Use raw events when debugging one path.  Use summary when comparing planner
regimes or detecting pressure.
