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
                       (accepted/rejected/displaced/skipped attribution)
pathprobe_probe2.c     join-level observation
pathprobe_json.c       observation wrappers pathprobe()/pathprobe_json()
                       and the machine-readable JSON renderer
pathprobe_propose.c    pathprobe_propose()/pathprobe_propose_json();
                       CustomPath injection, candidate and hypothetical
                       index fate, index-build hook consumer, hypothetical
                       indexes, advisor, pathkeys claim,
                       authorization call-out, lazy label provider registration
pathprobe_security.c   ACL-aware authorization (pathprobe_authorize_query,
pathprobe_security.h   hypo relation/column checks); SECURITY INVOKER,
                       delegates SELECT to core ExecCheckPermissions
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

The core patch's `pathprobe_index_path_hook` is not installed here: the
propose entry point sets it for the duration of one probe and restores it
afterwards (also on error).  It is compiled in only when the server headers
define `HAVE_PATHPROBE_INDEX_PATH_HOOK`, so an older copy of the core patch
still builds, without index-build coverage.

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
PathEventKind     SKIPPED / REJECTED / ACCEPTED / DISPLACED / SURVIVED /
                  EXCLUDED
PathEventLog      the per-run event buffer + relids pool + truncation flag
                  + coverage (PP_COV_* bits, fixed when the log is created;
                  the propose entry point adds PP_COV_INDEX_BUILD)
PathProbeReport   log + summary + advisor fields returned to the renderers
```

Path identity (`pelog_path_id`, pathprobe.c): a hash map from Path pointer
to a per-log id, plus a node table (`PathNode`: type, relids, child edges)
that the JSON renderer emits as `paths[]`.  Children of join, single-input
wrapper and multi-input paths are registered with their parent.  The probe1
hooks call `pelog_path_forget` for every path `add_path()` is about to free,
also when acceptance is not logged, so a recycled address gets a new id.
The map entry also keeps the seq of the path's last ACCEPTED event and the
rel it went into (`pelog_path_accepted`), which the rejected hook uses for
`dominator_seq`.

`pelog_append` (pathprobe.c) is the single insertion point: it assigns a
strictly monotone `seq`, enforces `max_events` (setting `events_truncated`
instead of growing silently), and interns relids.  `pelog_build_summary`
aggregates events per relation, bounded by `max_summary_entries` with a
`truncated` flag.

## 5. Probe layers

```text
probe0  set_rel_pathlist_hook: survivors visible without the core patch
probe1  pathprobe_probe1.c: add_path frontier - PEK_REJECTED (lost to an
        existing path), PEK_ACCEPTED (entered the pathlist) followed by one
        PEK_DISPLACED per path it removed (from the dominated list the core
        patch keeps alive until the hook returns), both only with
        pathprobe.log_accepted, and precheck PEK_SKIPPED; each event asks
        pathprobe_path_label() for an optional label
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

Candidate fate (`CandFate`, `propose_candidate_fate()`): replays the
candidate's own events (matched by label) in order, together with the log's
coverage.  Not injected gives `CAND_TARGET_NOT_FOUND` or `CAND_NOT_EXAMINED`
(the relation is in the query: `target_in_query`); otherwise REJECTED,
ACCEPTED, SURVIVED and DISPLACED at the base rel and ACCEPTED at an upper rel
give `rejected`, `displaced`, `survived` (or `survived_at_point` when
displacement is not covered), `survived_upper`, `unknown_after_accept` or
`unknown`.  A sequence that contradicts a covered point is `CAND_ILLEGAL`,
rendered `inconsistent` (JSON) / `INCONSISTENT (illegal transition)` (text),
with the offending seq.  A missing event never moves the state.  The text
card, `candidates[]` and the legacy top-level fate use this one function.
Fate is reported unreliable when the event log was truncated.

## 7. Hypothetical indexes

`hypo_*` (pathprobe_propose.c) support `hypo_indexes` (JSON column form) and
`hypo_sql` (full `CREATE INDEX` grammar, real parser).  Hypothetical index
paths carry `hypo:<name>` labels and participate in normal `add_path`
competition; none is ever built on disk.

`hypo_index_build_hook()` receives every `build_index_paths()` decision
during the probe and records those for a hypothetical index (pointer match
on the `IndexOptInfo` pathprobe built): calls per call site (plain,
non-native SAOP bitmap, OR arm), calls that generated paths, and the reasons
of the empty ones.  `hypo_fate()` combines that with the labeled events at
the relation the index was injected into (matched by planner relids) and
with coverage; see `event-model.md`.

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
points, `candidates[]` (with `fate` and `event_counts`), `advisor`, and
`hypo_indexes[]` (with `in_plan`, event counts, `fate`, `fate_reason` and
`index_build`), plus `coverage`.  User-controlled JSON strings must be
rendered with `pathprobe_json_str`: query text, identifiers, candidate names
and target names use this escaper.  Keep this invariant for any new JSON
string field.

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

For a superuser, `pathprobe_propose*` (advisor and hypothetical indexes
included) keeps full behavior.  For a non-superuser the propose surface runs
in a restricted, ACL-aware mode implemented in `pathprobe_security.c`
(SECURITY INVOKER, so `GetUserId()` is the caller):

- exactly one `SELECT` statement (multi-statement and non-`SELECT` refused);
- `pathprobe_authorize_query()` authorizes every `Query` level via
  `query_tree_walker` (subqueries, CTEs, sublinks, set-op arms), delegating
  relation/column `SELECT` to core `ExecCheckPermissions()` over the rewritten
  `rteperminfos` - no parallel privilege model;
- fail-closed on row-level security and on any RTE whose `checkAsUser`
  differs from the caller (a view/rule exposing a base relation the caller
  cannot select);
- `hypo_sql` denied; structured hypothetical indexes are btree-only and each
  target relation must be in the authorized set
  (`pathprobe_check_hypo_relation`) with per-column `SELECT`
  (`pathprobe_check_hypo_columns`, table-level SELECT honored first, then
  `pg_attribute_aclcheck`).

Planner hints change `enable_*` GUCs and are applied only *after*
authorization succeeds.  Observation (`pathprobe`/`pathprobe_json`) is
unrestricted.  See the README security model and
[safety-and-limits.md](safety-and-limits.md).

## 12. Build modes: stock vs patched PostgreSQL

```text
stock PostgreSQL
  reduced observability through the extension hooks available in core
  (probe0 survivors, join-level probe2); no add_path/precheck lifecycle

patched PostgreSQL (patches/pathprobe-core-hooks.patch)
  full add_path/precheck lifecycle observation via probe1
  built with -DHAVE_PATHPROBE_HOOKS
  build_index_paths() decisions for hypothetical indexes, when the server
  headers define HAVE_PATHPROBE_INDEX_PATH_HOOK
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
  -> parse spec JSON
  -> register label provider lazily
  -> (per statement) analyze + rewrite
  -> authorize: pathprobe_authorize_query (all Query levels, caller privileges)
  -> hypo relation + column checks (non-superuser)
  -> apply planner hints (only after authorization)
  -> install proposed / hypothetical / advisor candidates
  -> run planner (candidates compete in real add_path; the index path hook
     records build decisions for hypothetical indexes)
  -> compute candidate and hypothetical index fate from recorded events
     and coverage
  -> emit JSON evidence
```
