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

Hypothetical index probing can expose column names and planner statistics.
`pathprobe_propose*` (and therefore the advisor and `hypo_sql`) is
superuser-only in this release, as a conservative safety gate.  ACL-aware
access for non-superuser roles - restricted to queries and relations the
caller may already inspect - is the target model and is deferred; see the
roadmap in the README.

## Build capability

A stock PostgreSQL build and a patched build may expose different lifecycle
stages.  Tests and docs should use capability flags instead of pretending all
sources are always available.
