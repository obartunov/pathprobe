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
```

These are not synonyms.  They name different stages of path elimination.

## Example

```text
{1}  SeqScan        accepted=1  survived=1  best=145.00
{1}  IndexScan      rejected=1  survived=0
{1}  BitmapHeapScan accepted=1  displaced=1 survived=0
```

A plain reading:

```text
SeqScan survived.
IndexScan was built, but lost.
BitmapHeapScan entered the frontier, then was later displaced.
```

This is different from saying "PostgreSQL did not consider the index".  If an
`IndexScan` has a `rejected` event, PostgreSQL did consider it.

## Sources

`src` identifies the instrumentation source:

```text
pc  precheck stage, before a materialized Path exists
p1  add_path() stage, after a Path was built
p0  base-relation frontier snapshot
j2  join-level pre-generation / pair-local frontier observation
wp  proposed path or hypothetical path event
```

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
