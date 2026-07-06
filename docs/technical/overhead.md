# Overhead

pathprobe has three distinct overhead regimes.  They are documented
precisely here because the honest answer is *not* "zero overhead when
inactive".

## 1. Not loaded: zero overhead

If the library is not loaded (no `shared_preload_libraries`, no `LOAD`,
no extension in the session), pathprobe installs no hooks and runs no
code.  Overhead is exactly zero.

## 2. Loaded but inactive: hook dispatch + fast guards

Once the library is loaded, it installs planner and EXPLAIN hooks
(`set_rel_pathlist_hook`, `add_path` observation via the core patch,
join-level hooks, `explain_per_plan_hook`).  These hooks stay installed
for the life of the backend.

For a query with no active probe, each hook still fires and then returns
early on a fast guard:

```c
if (pathprobe_active_log == NULL)
    return;                     /* probe1, probe2, rel-pathlist hooks */
```

probe2 additionally checks `!pathprobe_enable_probe2`.  The cost is hook
dispatch plus a NULL/GUC comparison per hook invocation.  It is small but
not zero, and it is **planning-time only** - execution is not touched.

Consequence: a loaded pathprobe adds a fixed, tiny planner dispatch cost
to every planned statement in that backend, whether or not a probe runs.

## 3. Active probe: pathprobe / pathprobe_json / EXPLAIN pathprobe

When a probe is active, the hooks do real work: record path events,
intern/copy relids, build the summary table, and render text or JSON.

Cost grows with:

```text
number of relations
number of join pairs explored
number of paths generated
number of events recorded
output size (text/JSON rendering)
```

This is bounded by `pathprobe.max_events` and
`pathprobe.max_summary_entries`; when a bound is hit the output is
truncated and the truncation is reported, never silent.

## 4. probe1 / add_path tracing

probe1 (accepted/rejected/displaced events on the `add_path` frontier) is
the most detailed and potentially highest-volume source: a single
relation can see many candidate paths accepted and later displaced.

For this reason `pathprobe.log_accepted` is **off by default** - accepted
events are the highest-volume kind, and most diagnostics only need the
rejected/survived picture.  Turn it on deliberately, for a targeted query.

## 5. propose / advisor / hypo: intentionally more expensive

`pathprobe_propose`, the advisor, and hypothetical indexes deliberately do
extra work: they run additional planning attempts, inject proposed and
hypothetical paths, and derive advisor candidates.  This is more expensive
than plain observation by design.

These modes must be bounded with:

```text
pathprobe.max_events
pathprobe.max_advisor_candidates
pathprobe.max_hypo_indexes
pathprobe.max_query_length
```

`pathprobe.agent_mode` clamps the effective event and advisor caps for
autonomous callers.

## 6. Agent rule: truncation is not green

For an autonomous caller, a truncated result is a degraded result: the
picture may be incomplete and a gate decision made on it is unsafe.  The
reporter treats any truncation (`limits.events_truncated`,
`summary_truncated`, advisor `truncated`) as **red**, not green.  An agent
must not treat a truncated probe as a clean bill of health.

## Practical guidance

pathprobe is designed so expensive work happens only when a probe is
active, but a loaded extension still adds planner hook dispatch overhead.
Use it for targeted diagnostics, tests, and agent preflight - not as
blanket tracing for every production query.
