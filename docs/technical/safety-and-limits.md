# Safety and limits

`pathprobe` is diagnostic planner instrumentation.  Agent use requires hard
limits and explicit incomplete-output flags.

## Relevant GUCs

```text
pathprobe.agent_mode
pathprobe.log_accepted
pathprobe.min_gap
pathprobe.enable_probe2
pathprobe.max_events
pathprobe.max_summary_entries
pathprobe.max_advisor_candidates
pathprobe.max_hypo_indexes
pathprobe.max_query_length
```

## Agent-safe rules

A SQL agent should:

```text
use JSON, not text
use planning/propose modes before execution
set max_events and max_advisor_candidates
treat truncation as non-green
treat unknown/inconsistent candidate fate as non-green
never execute a proposed path
route heavy read-only queries away from primary when policy says so
```

## Truncation

Truncation must be visible in JSON:

```json
"limits": {
  "events_truncated": true,
  "summary_truncated": false,
  "max_events": 16
}
```

Silent truncation is unsafe.  If a limit was hit, the picture is incomplete.

## Proposed path execution boundary

`winner_forbidden=true` is the normal diagnostic mode.

`winner_forbidden=false` is a boundary test.  A proposed path may win planning,
but it must not silently reach real execution.  Explicit ERROR is the expected
safe behavior.

## Permission boundary

Hypothetical index probing and planner output can expose column names and
planner statistics.  Superusers get full behavior.  Non-superusers run in a
restricted, ACL-aware mode: a single `SELECT` probe, every `Query` level
authorized under the caller's own privileges (relation and column `SELECT`
delegated to core `ExecCheckPermissions()`), row-level security and
view/rule `checkAsUser` exposure failing closed, `hypo_sql` denied, and
structured hypothetical indexes limited to btree on relations and columns the
caller is authorized for.

Not supported yet: RLS-compatible mode, owner-level `hypo_sql`, view-safe
redaction, and expression/predicate hypothetical indexes for non-superusers.
The security boundary is exercised by the Perl TAP model tests
(`t/010_pathprobe_model_security.pl`, see
`docs/testing/model-based-tests.md`).

## Build capability

A stock PostgreSQL build and a patched build may expose different lifecycle
stages.  Tests and docs should use capability flags instead of pretending all
sources are always available.
