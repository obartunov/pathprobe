# Implementation

This document describes how the current code implements the model in
[architecture.md](architecture.md).  It is file-oriented: what each source
file is responsible for and how the pieces connect.

`pathprobe` observes planner alternatives.  It does not execute alternative
plans, and it does not force the final plan.  Proposed paths are diagnostic
candidates, not execution promises.

## 1. Source file map

```text
pathprobe.c            _PG_init, GUCs, hook installation, event log,
                       summary, text rendering, label provider setter/accessor
pathprobe.h            shared structs (PathEvent, PathEventLog,
                       PathProbeReport), enums, label provider API, GUC externs
pathprobe_probe1.c     add_path/precheck lifecycle instrumentation
                       (accepted/rejected/skipped/survived attribution)
pathprobe_probe2.c     join-level observation
pathprobe_json.c       observation wrappers pathprobe()/pathprobe_json()
                       and the machine-readable JSON renderer
pathprobe_propose.c    pathprobe_propose()/pathprobe_propose_json();
                       CustomPath injection, candidate fate FSM,
                       hypothetical indexes, advisor, pathkeys claim,
                       ACL gate, lazy label provider registration
tools/pathprobe_report.py   deterministic post-processing (--gate/--advisor/--agent)
```

## 2. Runtime entry points

Four SQL-callable C functions:

```text
pathprobe(query text)                    pathprobe_json.c  (text observation)
pathprobe_json(query text)               pathprobe_json.c  (JSON observation)
pathprobe_propose(query, spec)           pathprobe_propose.c (text, injection)
pathprobe_propose_json(query, spec)      pathprobe_propose.c (JSON, injection)
```

The observation wrappers (pathprobe() is text, pathprobe_json() is JSON) live in `pathprobe_json.c`; the propose entry
points live in `pathprobe_propose.c`.  The event-log, summary, and text
renderer they call are in `pathprobe.c`.

## 3. Hook installation

`_PG_init` (pathprobe.c) installs the hooks once per backend and registers
the GUCs.  With the core patch (`HAVE_PATHPROBE_HOOKS`) it chains the
`add_path` / precheck hooks used by probe1; without it, only the
stock-available hooks are installed (reduced coverage).  probe2's join-level
hooks are always installed; the `pathprobe.enable_probe2` GUC gates their
work inside the hook body, so toggling it at runtime is safe.

`_PG_init` never references the propose layer: the path label provider is
registered lazily from the propose entry points (section 6), keeping
`_PG_init` free of propose knowledge.

GUCs registered here:

```text
pathprobe.explain            EXPLAIN integration on/off
pathprobe.log_accepted       record PEK_ACCEPTED events (off by default)
pathprobe.min_gap            gap threshold for reporting
pathprobe.enable_probe2      join-level observation on/off
pathprobe.agent_mode         conservative caps for autonomous callers
pathprobe.max_events         hard cap on recorded events
pathprobe.max_summary_entries
pathprobe.max_advisor_candidates
pathprobe.max_hypo_indexes
pathprobe.max_query_length
```

## 4. Event log model

`pathprobe.h` defines the shared structures:

```text
PathEvent         one path life event: kind, source, origin, relids,
                  pathtype, costs, optional label
PathEventKind     SKIPPED / REJECTED / ACCEPTED / DISPLACED / SURVIVED
PathEventLog      the per-run event buffer + relids pool + truncation flag
PathProbeReport   log + summary + advisor fields returned to the renderers
```

`pelog_append` (pathprobe.c) is the single insertion point: it assigns a
strictly monotone `seq`, enforces `max_events` (setting `events_truncated`
instead of growing silently), and interns relids.  `pelog_build_summary`
aggregates events per relation, bounded by `max_summary_entries` with a
`truncated` flag.

## 5. Probe layers

```text
probe0  set_rel_pathlist_hook: survivors visible without the core patch
probe1  pathprobe_probe1.c: add_path frontier - PEK_REJECTED (displaced or
        losing), PEK_ACCEPTED (entered frontier, off by default), and
        precheck PEK_SKIPPED; each event asks pathprobe_path_label() for an
        optional label
probe2  pathprobe_probe2.c: join-level observation (setup/pathlist/search),
        gated by pathprobe.enable_probe2
```

All probes fast-return when `pathprobe_active_log == NULL`.

## 6. Proposed path injection

`pathprobe_propose*` (pathprobe_propose.c) parse a JSON `spec`, then inject a
diagnostic `CustomPath` (`proposed_path_methods`) into real `add_path`
competition.  The candidate competes exactly like any other path; the code
never executes it.  The guard sits at the true execution boundary:
`CreateCustomScanState` errors out, so a proposed path that somehow won
planning cannot run.

Lazy label registration: each propose entry point calls
`pathprobe_propose_register_label_provider()`, which sets
`pathprobe_proposed_pathname` as the resolver via
`pathprobe_set_path_label_provider()`.  This is backend-local and idempotent;
probe1 then attributes `hypo:<name>` / candidate labels through the accessor
without pathprobe.c depending on propose symbols.

Candidate fate FSM (`CandFateState`, `propose_candidate_fsm`): six states -
`CAND_INJECTED`, `CAND_ACCEPTED`, `CAND_REJECTED`, `CAND_SURVIVED`,
`CAND_UPPER`, `CAND_ILLEGAL`.  It replays the candidate's own path events
(matched by label) in order:

```text
INJECTED --PEK_ACCEPTED(base rel)--> ACCEPTED
ACCEPTED --PEK_SURVIVED-------------> SURVIVED
{ACCEPTED,SURVIVED,UPPER} --PEK_ACCEPTED(upper: empty relids)--> UPPER
INJECTED --PEK_REJECTED-------------> REJECTED
any other transition ---------------> ILLEGAL (records the offending seq)
```

`CAND_ILLEGAL` is the enum state for an impossible transition; it renders as
`inconsistent` (JSON) / `INCONSISTENT (illegal transition)` (text).  Fate is
derived purely from recorded events, and is reported unreliable when the
event log was truncated.

## 7. Hypothetical indexes

`hypo_*` (pathprobe_propose.c) support `hypo_indexes` (JSON column form) and
`hypo_sql` (full `CREATE INDEX` grammar, real parser).  Hypothetical index
paths carry `hypo:<name>` labels and participate in normal `add_path`
competition; none is ever built on disk.

### AM support (btree, gin, gist, brin, spgist)

Support for non-btree AMs is not special-cased per AM.  It is a single
abstraction: `hypo_fill_am(ioi, relam)` asks the real AM routine
(`GetIndexAmRoutineByAmId`) for its capabilities and copies them onto the
hypothetical `IndexOptInfo` - `amcanorderbyop`, `amoptionalkey`,
`amsearcharray`, `amsearchnulls`, `amcanparallel`, gettuple/getbitmap/
markpos availability, and crucially `amcostestimate`.  Both index builders
(the `CREATE INDEX` form and the legacy column form) go through this one
function, so adding an AM needs no new code here - the AM describes itself.
The stock per-AM cost estimators then run unchanged:

```text
h->amoid            resolved from the CREATE INDEX access method via
                    get_index_am_oid (InvalidOid/btree default)
ResolveOpClass      per-column opclass resolved against the actual AM,
                    not assumed btree
hypo_fill_am        copies capability fields from the AM's amroutine:
                    amcanorderbyop, amcanorder; sortopfamily is set only
                    for amcanorder AMs (mirrors plancat.c); canreturn is
                    true only for btree, false for other AMs until a real
                    index would prove otherwise
```

Two concrete correctness points:

1. `opclassoptions` is allocated and zero-filled (`palloc0`) for every
   column in both index builders.  The gin cost estimator dereferences
   `opclassoptions` (via `set_fn_opclass_options`); leaving it NULL is a
   crash.  Zero-filled means "no options", which is the correct default
   for an index with no opclass parameters.

2. Costing uses each AM's own estimator.  The core already guards
   `hypothetical` indexes in the gin and brin estimators (it invents their
   statistics); gist and spgist cost generically.  The size estimate stays
   btree-shaped and is reported as `physics: modeled`, since a real
   relation would be needed for an exact size.

The execution boundary is unchanged: a hypothetical path can win planning
but cannot be executed (section 6).

## 8. Advisor

The advisor (`adv_*`, pathprobe_propose.c) derives candidate indexes from a
query's restriction/equivalence atoms, generates single/composite/covering/
ordered index candidates, and skips prefixes of existing indexes when
`advise_skip_prefix` is set.  Each candidate becomes a hypothetical index and
is tested through real planner competition, bounded by
`max_advisor_candidates`.  The advisor derives evidence; it is not a
cost-model of its own.

## 9. JSON rendering

`pathprobe_json.c` renders the machine-readable output: `events` (with
`label` and `origin`), `summary`, `limits`, and, for the propose entry
points, `candidates[]`, `advisor`, and `hypo_indexes[]` (with explicit
`in_plan` boolean and `event_counts`).  All string fields go through a single
escaper (`pathprobe_json_str`) so arbitrary query text and identifiers are
safely encoded.

## 10. Limits and truncation

Every bound is reported, never silent:

```text
max_events            -> limits.events_truncated
max_summary_entries   -> limits.summary_truncated
max_advisor_candidates-> advisor.truncated
max_hypo_indexes      -> ERROR (explicit) on oversized user input
max_query_length      -> ERROR (explicit) on oversized query
```

`agent_mode` clamps the effective event and advisor caps locally without
mutating the GUC variables.

## 11. Security boundary

`propose_acl_gate()` restricts `pathprobe_propose*` (and therefore the
advisor and hypothetical indexes) to superusers.  Planning-only probing can
expose statistics for relations the caller may not inspect, so the propose
surface is superuser-only in this release; observation
(`pathprobe`/`pathprobe_json`) is unrestricted.  See the README security
status and [safety-and-limits.md](safety-and-limits.md).

## 12. Build modes: stock vs patched PostgreSQL

```text
stock PostgreSQL
  reduced observability through the extension hooks available in core
  (probe0 survivors, join-level probe2); no add_path/precheck lifecycle

patched PostgreSQL (patches/pathprobe-core-hooks.patch)
  full add_path/precheck lifecycle observation via probe1
  built with -DHAVE_PATHPROBE_HOOKS
```

`_PG_init` selects the installed hook set with `#ifdef HAVE_PATHPROBE_HOOKS`.
See [../../patches/APPLYING.md](../../patches/APPLYING.md).

## Flow: pathprobe() / pathprobe_json()

```text
SQL entry point (pathprobe_json.c)
  -> create PathEventLog
  -> run planner
  -> probe1/probe2 collect events (fast-return when no active log)
  -> build summary
  -> render text / JSON (pathprobe.c / pathprobe_json.c)
```

## Flow: pathprobe_propose_json()

```text
SQL entry point (pathprobe_propose.c)
  -> ACL gate (superuser)
  -> parse spec JSON
  -> register label provider lazily
  -> install proposed / hypothetical / advisor candidates
  -> run planner (candidates compete in real add_path)
  -> compute candidate fate from recorded events
  -> emit JSON evidence
```
