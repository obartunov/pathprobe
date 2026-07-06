> ARCHIVED (v0.1). Superseded by docs/user/proposed-paths.md and the
> current test suites. Kept for history; the manual CREATE FUNCTION flow
> and JSON shapes here are outdated.

# Proposed Path Testing Note v0.1

Target: pathprobe v1.2, PostgreSQL master (20devel + core patch)  
Tester: @solo  
Reviewer: @yoda  

Main thesis:
  We are not only testing whether proposed paths can be injected;
  we are testing whether their planner fate is real, visible, and
  interpreted correctly under policy.

Pass criteria — all three must hold simultaneously:
  1. planner behavior matches the scenario
  2. output explains that behavior correctly
  3. policy boundary remains explicit

Fail conditions:
  - proposed candidate injected but invisible (wp events absent)
  - candidate visible but not actually participating in add_path()
  - acc=1, surv=0 under policy looks like ordinary rejection (no note)
  - pathname lost in summary or JSON
  - strong candidate cannot win even with winner_forbidden=false
  - strong candidate silently executes (must raise explicit error)
  - gate matrix disagrees with raw event log

---

## Setup

```sql
LOAD 'pathprobe';

-- Required functions
CREATE OR REPLACE FUNCTION pathprobe_propose(query text, spec text)
    RETURNS text AS 'pathprobe','pathprobe_propose' LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION pathprobe_propose_json(query text, spec text)
    RETURNS text AS 'pathprobe','pathprobe_propose_json' LANGUAGE C STRICT;

-- Test table with index so planner has real competition
CREATE TABLE IF NOT EXISTS orders (
    id int PRIMARY KEY,
    customer_id int,
    amount numeric,
    order_date date
);
CREATE INDEX IF NOT EXISTS orders_customer_id_idx ON orders(customer_id);
INSERT INTO orders SELECT i,(i%100)+1,(random()*1000)::numeric,
    current_date-(i%365) FROM generate_series(1,10000) i ON CONFLICT DO NOTHING;
ANALYZE orders;
```

---

## Layer 1 — Injection Correctness

### T1.1 smoke — weak proposed loser, policy off

Scenario: injection is real but harmless when candidate is clearly weak.

```sql
SET pathprobe.log_accepted = on;
SELECT pathprobe_propose(
    'SELECT * FROM orders WHERE customer_id = 42',
    '{
        "target":    {"relname": "orders"},
        "candidate": {"pathname": "smoke_loser", "provider": "test"},
        "cost":      {"startup_cost": 0.5, "total_cost": 9999.0,
                      "rows": 100, "width": 48},
        "planner":   {"parameterized": false, "parallel_safe": true},
        "mode":      {"winner_forbidden": false}
    }'
);
```

Expected planner semantics:
  - wp events present: src=wp, kind=rejected
  - ordinary winner unchanged (IndexScan or BitmapHeapScan)

Expected gate matrix row:
  wp:smoke_loser   ○  ○  ✗  ─  ─   rejected

Expected JSON:
  fate.fate = "rejected"
  events contains {src:"wp", kind:"rejected"}
  candidate.pathname = "smoke_loser"

Pass/fail note:
  FAIL if wp events absent from log
  FAIL if ordinary winner changed
  FAIL if pathname missing from output


### T1.2 smoke — weak proposed loser, policy on

Same as T1.1 but winner_forbidden=true.
disabled_nodes=1 causes immediate rejection before cost competition.

Expected:
  - same rejection, same ordinary winner
  - note says "disabled_nodes=1 caused immediate rejection"

---

## Layer 2 — Planner Semantics

### T2.A loser_basic

```sql
SET pathprobe.log_accepted = on;
SELECT pathprobe_propose_json(
    'SELECT * FROM orders WHERE customer_id = 42',
    '{
        "target":    {"relname": "orders"},
        "candidate": {"pathname": "idx_like_loser", "provider": "whatif"},
        "cost":      {"startup_cost": 0.20, "total_cost": 40.00,
                      "rows": 120, "width": 48},
        "planner":   {"parameterized": false, "parallel_safe": true},
        "mode":      {"winner_forbidden": true}
    }'
);
```

Expected planner semantics:
  - path injected and participates in add_path()
  - kind=rejected (disabled_nodes=1 causes loss to any non-disabled path)
  - no fake survived

Expected gate matrix row:
  wp:idx_like_loser   ○  ○  ✗  ─  ─   rejected   raw=40.00   winner=<builtin>@<cost>

Expected JSON fields:
  fate.fate = "rejected"
  fate.winner_strategy present
  fate.gap < 0 (candidate cheaper on raw cost)
  fate.note = "disabled_nodes_caused_rejection"
  events: {src:"wp", kind:"rejected", pathtype:"CustomScan"}

FAIL if fate.fate = "unknown"
FAIL if wp events absent


### T2.B accepted_then_forbidden

```sql
SELECT pathprobe_propose_json(
    'SELECT customer_id, amount FROM orders WHERE customer_id = 42',
    '{
        "target":    {"relname": "orders"},
        "candidate": {"pathname": "duckdb_scan_filter", "provider": "duckdb"},
        "cost":      {"startup_cost": 0.80, "total_cost": 18.00,
                      "rows": 50, "width": 40},
        "planner":   {"parameterized": false, "parallel_safe": true},
        "mode":      {"winner_forbidden": true}
    }'
);
```

Expected planner semantics:
  - path enters frontier: acc=1
  - does not become cheapest_total_path: surv=0
  - this is policy-forced, not ordinary weak path

Expected gate matrix row:
  wp:duckdb_scan_filter   ○  ○  ○  ✗  ⊘   forbidden ⊘   raw=18.00   winner=<builtin>@<cost>

Expected JSON:
  fate.fate = "accepted_not_survived"
  fate.note = "candidate_would_win_policy_prevented"
  fate.gap < 0

FAIL if fate.fate = "rejected" (means candidate never entered frontier)
FAIL if note absent or generic
FAIL if pol column shows ─ instead of ⊘


### T2.C clearly_best_but_forbidden

```sql
SELECT pathprobe_propose_json(
    'SELECT * FROM orders WHERE customer_id = 42',
    '{
        "target":    {"relname": "orders"},
        "candidate": {"pathname": "forbidden_winner", "provider": "whatif"},
        "cost":      {"startup_cost": 0.01, "total_cost": 1.00,
                      "rows": 10, "width": 48},
        "planner":   {"parameterized": false, "parallel_safe": true},
        "mode":      {"winner_forbidden": true}
    }'
);
```

Expected:
  - candidate best on raw cost
  - ordinary path remains effective winner
  - raw_best and eff_win columns show the divergence

Expected gate matrix row:
  wp:forbidden_winner   ○  ○  ○  ✗  ⊘   forbidden ⊘   raw=1.00   winner=<builtin>@<cost>

Expected JSON:
  fate.gap < 0 (large negative — clearly would win)
  fate.winner_cost > 1.00
  candidate.cost_total = 1.00

FAIL if raw_best = eff_win (they must diverge)
FAIL if effective winner changed to proposed candidate


### T2.D clearly_best_WITHOUT_policy (execution boundary test)

IMPORTANT: this test proves proposed path is a real planner participant.

```sql
\set ON_ERROR_STOP off
SELECT pathprobe_propose(
    'SELECT * FROM orders WHERE customer_id = 42',
    '{
        "target":    {"relname": "orders"},
        "candidate": {"pathname": "forbidden_winner", "provider": "whatif"},
        "cost":      {"startup_cost": 0.01, "total_cost": 1.00,
                      "rows": 10, "width": 48},
        "planner":   {"parameterized": false, "parallel_safe": true},
        "mode":      {"winner_forbidden": false}
    }'
);
\set ON_ERROR_STOP on
```

Expected:
  ERROR: pathprobe: proposed path "forbidden_winner" (provider: whatif)
         reached execution — this is a diagnostic-only candidate
  HINT:  Use winner_forbidden=true to prevent the proposed path from winning

FAIL if no error (means candidate did not win — check costs)
FAIL if error message does not name the pathname and provider
FAIL if silent success (catastrophic: fake execution)


---

## Layer 3 — Presentation Correctness

### T3.1 gate matrix symbols vs raw event log

For each of T2.A / T2.B / T2.C, verify manually:

```python
# Save JSON:
# psql -t -A -c "SELECT pathprobe_propose_json(...)" > propose.json

# Gate matrix:
python pathprobe_report.py propose.json --gate

# Raw wp events for cross-check:
python pathprobe_report.py propose.json --src wp
```

Verification rules:
  - if raw events show kind=rejected → gate matrix must show ✗ at cmp, ─ at fr and pol
  - if raw events show kind=accepted (no survived) → gate matrix: ○ at cmp, ✗ at fr, ⊘ at pol
  - if pol column shows ⊘ → fate.note must mention policy
  - pathname must appear as wp:<pathname> in gate matrix row

### T3.2 JSON field completeness

For T2.B (accepted_then_forbidden), verify JSON output:

Required fields:
  candidate.pathname          non-empty string
  candidate.provider          non-empty string
  candidate.winner_forbidden  boolean true
  candidate.cost_total        float
  fate.fate                   "accepted_not_survived"
  fate.winner_strategy        non-empty string
  fate.winner_cost            float > 0
  fate.gap                    float < 0
  fate.note                   non-empty string
  events[*].src               contains "wp" entries
  events[*].kind              "accepted" for wp entries

FAIL if any required field absent
FAIL if pathname absent from any wp event

### T3.3 help / docstring consistency

```bash
python pathprobe_report.py -h 2>&1 | grep -E "gen=|pre=|cmp=|fr=|pol="
# Must output: gen=generate  pre=precheck  cmp=compete  fr=frontier  pol=policy

python pathprobe_report.py -h 2>&1 | grep "GATE SYMBOLS"
# Must output: ○ passed  ✗ first failing gate  ─ not reached  ⊘ policy blocked
```

FAIL if gate column names in -h differ from actual matrix output
FAIL if symbols in -h differ from symbols in rendered matrix footer

---

## Layer 4 — Safety Boundary

### T4.1 winner_forbidden=true keeps candidate diagnostic-only

After T2.B or T2.C:
  - verify ordinary query still executes correctly
  - proposed path must NOT appear in EXPLAIN of ordinary queries
  - no contamination of planner state after pathprobe_propose_json() call

```sql
-- Run proposed path experiment
SELECT pathprobe_propose_json(
    'SELECT * FROM orders WHERE customer_id = 42',
    '{ ... winner_forbidden:true ... }'
);

-- Immediately after: ordinary plan must be clean
EXPLAIN SELECT * FROM orders WHERE customer_id = 42;
-- Must NOT mention CustomScan or ProposedPath
```

### T4.2 winner_forbidden=false → explicit execution error

Covered by T2.D. Key point: error must be raised at execution, not at planning.

Planning must succeed (proposed path wins).
Execution must fail with named diagnostic.

### T4.3 no silent fallback

If proposed path cannot be injected (target rel not found):

```sql
SELECT pathprobe_propose(
    'SELECT * FROM orders',     -- no WHERE, different query shape
    '{"target":{"relname":"nonexistent_table"}, ... }'
);
```

Expected output must contain:
  WARNING: target relation not found in query

FAIL if output silently proceeds as if injection succeeded.

---

## Minimal Regression Set (6 tests)

| # | test         | winner_forbidden | expected fate       | key check               |
|---|--------------|-----------------|---------------------|-------------------------|
| 1 | smoke_loser  | false           | rejected            | wp events present       |
| 2 | loser_policy | true            | rejected            | disabled_nodes note     |
| 3 | accepted_forbidden | true     | accepted_not_survived | pol=⊘, gap<0         |
| 4 | best_forbidden | true          | accepted_not_survived | raw≠eff_win           |
| 5 | best_no_policy | false         | ERROR at execution  | named error, no silence |
| 6 | duckdb_metadata | true         | accepted_not_survived | pathname+provider in JSON |

Test #6 spec fragment:
```json
{
  "candidate": {"pathname": "duckdb_filter_agg", "provider": "duckdb",
                "pathtype": "CustomPath"},
  "pushdown":  {"scan": true, "filter": true, "aggregate": true},
  "residual":  {"filter": false}
}
```
Verify: pathname and provider appear in gate matrix and JSON fate block.
pushdown/residual fields stored in custom_private and visible in future debug mode.

---

## Quick run script

```bash
#!/bin/bash
# Run all 6 regression tests and show gate matrix for each
PSQL="psql -t -A postgres"

for spec_file in t1_smoke t2_loser t3_accepted t4_best t5_nopolicy t6_duckdb; do
    echo "=== $spec_file ==="
    $PSQL -c "SELECT pathprobe_propose_json(...)" > /tmp/${spec_file}.json
    python pathprobe_report.py /tmp/${spec_file}.json --gate
done
```

---

## Notes for @solo

- Always use tables with real indexes (orders_customer_id_idx) so planner
  generates multiple competing paths; otherwise only SeqScan appears and
  competition is trivial.

- T2.D requires \set ON_ERROR_STOP off in psql or try/except in Python.

- For T3.1: cross-check gate matrix against --src wp raw trace.
  If they disagree, the bug is in retag_proposed_events() or render_propose_card().

- fate.fate = "unknown" always indicates missing wp events in log.
  Cause: log_accepted=false and only probe0 active.
  Fix: ensure pathprobe_propose_json() forces log_accepted=true internally.

