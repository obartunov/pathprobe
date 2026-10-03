# Examples: why pathprobe, if PostgreSQL has EXPLAIN

```text
EXPLAIN    the plan that survived
pathprobe  what the planner considered on the way: excluded, skipped,
           not examined, not generated, rejected, accepted, displaced,
           survived, or unknown because a decision point is not observed
```

| # | story | script |
|---|---|---|
| 1 | [EXPLAIN is not enough](01-explain-is-not-enough.md) | `examples/01_explain_is_not_enough.sql` |
| 2 | [Displaced paths](02-displaced-paths.md) | `examples/02_displaced_paths.sql` |
| 3 | [target_not_found, not_examined, unknown](03-target-not-found-not-examined-unknown.md) | `examples/03_target_not_found_not_examined_unknown.sql` |
| 4 | [Not generated index path](04-not-generated-index-path.md) | `examples/04_not_generated_index_path.sql` |
| 5 | [Hypothetical index lifecycle](05-hypothetical-index-lifecycle.md) | `examples/05_hypothetical_index_lifecycle.sql` |
| 6 | [Agent preflight](06-agent-preflight.md) | `examples/06_agent_preflight.sql` |
| 7 | [Advisor candidate lifecycle](07-advisor-candidate-lifecycle.md) | `examples/07_advisor_candidate_lifecycle.sql` |
| 8 | [Advisor limitations](08-advisor-limitations.md) | `examples/08_advisor_limitations.sql` |

All eight scripts are runnable.  The part of story 3 about builds without the
core hooks patch is description only.

## Running

The scripts need a superuser (`LOAD`, `pathprobe.log_accepted`,
`pathprobe.min_gap` and `hypo_sql` are superuser-only) and a server built
with `patches/pathprobe-core-hooks.patch`, including the index path hook.

```sh
PGDATABASE=scratch examples/run_all.sh
```

Each script creates its own small tables (`ex1_*` ... `ex8_*`, at most 30000
rows so that `ANALYZE` reads every row), runs
`ANALYZE`, prints what EXPLAIN and pathprobe show, and ends with structural
checks that raise an error when an expectation does not hold.  The checks
never compare cost values.  `max_parallel_workers_per_gather` is 0 in all of
them.

The excerpts in these pages were produced on PostgreSQL 20devel with the
tested core patch; cost numbers in them are illustrative only.
