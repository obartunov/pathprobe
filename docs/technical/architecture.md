# Architecture

For how the current code implements this model, see
[implementation.md](implementation.md).

`pathprobe` observes PostgreSQL planner decisions through a small event pipeline.

```text
source adapters / hooks
        ↓
PathEventLog
        ↓
PathProbeReport
        ↓
render_text / render_json / EXPLAIN / reporter
```

## Source adapters

Current source tags:

```text
pc  precheck-stage observation
p1  add_path() observation
p0  base-relation frontier snapshot
j2  join-level setup/pathlist observation
wp  proposed candidate (CustomScan) observation
```

Hypothetical index paths are observed by the regular adapters and carry a
`hypo:<name>` label; `build_index_paths()` decisions for them come from the
core index path hook and are summarized in `hypo_indexes[]`, not as events.

The patched PostgreSQL build exposes more lifecycle stages than a stock build.
Documentation and tests should distinguish capability, not pretend all builds
produce the same evidence.

## Canonical internal model

The append-only `PathEventLog` is the canonical data structure.

Renderers must not invent independent semantics.  They may present a subset or a
derived summary, but the meaning must come from the event stream.

## Derived report

`PathProbeReport` is derived from the log.  It may contain:

```text
raw events
relation map
summary rows
candidate fates
advisor block
hypothetical index summary
limits/truncation flags
```

## Proposed paths

Proposed candidates are diagnostic `CustomPath` candidates injected into real
planner competition.  They are labeled, so their fate is derived from the event
stream by identity, not by cost guessing.

A proposed path must never silently execute.  If a non-forbidden proposed path
wins, the executor boundary must raise an explicit error.

## Advisor / hypothetical indexes

Advisor-generated and user-supplied hypothetical indexes are represented as fake
planner index metadata.  PostgreSQL builds normal paths over them and competes
through ordinary planner machinery.

Events for such paths are labeled `hypo:<name>`.  The JSON `hypo_indexes[]` block
is a machine-oriented summary over those labels plus final-plan attribution.

## EXPLAIN integration

`pathprobe.explain = on` adds a `PathProbe` section to `EXPLAIN`.

FORMAT TEXT may use the human renderer.  FORMAT JSON/XML/YAML must be emitted
through PostgreSQL's structured `ExplainProperty` API so the result remains
machine-valid.
