# pathprobe BASELINE

Canonical output examples for pathprobe v1.2 (extension SQL level 1.1),
generated on PostgreSQL 20devel with the pathprobe core patch applied
(probe0+probe1+probe2 all active).

## Presentation hierarchy

1. `pathprobe(query)` / `pathprobe_json(query)` - full event log + summary.
2. `SET pathprobe.explain = on` - the same section inline in EXPLAIN
   (TEXT appended; FORMAT JSON/XML/YAML via ExplainProperty).
3. `pathprobe_propose(query, spec)` - counterfactual candidates and
   hypothetical indexes injected into the real competition.

## Gate view - ordinary pathprobe (with reverse-case labels)

Survivors kept although not cheapest-total carry the reason:
`kept: startup` (cheapest-startup) or `kept: order?` (carries a sort
order a parent may need; use not proven).

```
PathProbe  [probe0+probe1+precheck: full path life visible]
rels: 1=d  2=r  
-------------------------------------------------------------------
 seq  src  rel           pathtype              cost_total  gap                     event
-------------------------------------------------------------------
#1    p1  {1}           BitmapHeapScan              4.31                          REJECTED [outer]
#2    p0  {1}           IndexScan                   0.31                          survived [param]
#3    p0  {1}           BitmapHeapScan             57.15                          survived
#4    p0  {1}           IndexScan                 286.28                          survived  kept: startup
#5    p1  {2}           BitmapHeapScan             25.45                          REJECTED [outer]
#6    p0  {2}           IndexScan                  24.21                          survived [param]
#7    p0  {2}           SeqScan                  1042.00                          survived
#8    p0  {2}           IndexScan                2970.21                          survived  kept: order?
#9    pc  {1,2}         NestLoop                46947.15  dominated_early         SKIPPED
#10   p1  {1,2}         NestLoop                 1151.00                          REJECTED
#11   pc  {1,2}         NestLoop                 7999.15  dominated_early         SKIPPED
#12   pc  {1,2}         NestLoop                47176.28  dominated_early         SKIPPED
#13   p1  {1,2}         NestLoop                 1380.13                          REJECTED
#14   pc  {1,2}         NestLoop                 8228.28  dominated_early         SKIPPED
#15   pc  {1,2}         MergeJoin                6240.08  dominated_early         SKIPPED
#16   pc  {1,2}         MergeJoin                3256.49  dominated_early         SKIPPED
#17   pc  {1,2}         HashJoin                 2078.39  dominated_early         SKIPPED
#18   pc  {1,2}         HashJoin                 1849.26  dominated_early         SKIPPED
#19   j2  {1,2}         NestLoop                 1149.67                          survived
#20   j2  {1,2}         NestLoop                 1378.80                          survived  kept: startup
#21   pc  {1,2}         MergeJoin                6012.30  dominated_early         SKIPPED
#22   pc  {1,2}         NestLoop               278917.00  dominated_early         SKIPPED
#23   pc  {1,2}         NestLoop                17992.00  dominated_early         SKIPPED
#24   pc  {1,2}         NestLoop                 3902.81  dominated_early         SKIPPED
#25   p1  {1,2}         NestLoop                41594.74                          REJECTED
#26   pc  {1,2}         NestLoop               280845.21  dominated_early         SKIPPED
#27   pc  {1,2}         NestLoop                19920.21  dominated_early         SKIPPED
#28   pc  {1,2}         NestLoop                 5831.02  dominated_early         SKIPPED
#29   pc  {1,2}         NestLoop                 2974.84  dominated_early         SKIPPED
#30   pc  {1,2}         MergeJoin                3028.71  dominated_early         SKIPPED
#31   pc  {1,2}         MergeJoin                3256.49  dominated_early         SKIPPED
#32   pc  {1,2}         HashJoin                 1249.71  dominated_early         SKIPPED
#33   j2  {1,2}         NestLoop                 1149.67                          survived
#34   j2  {1,2}         NestLoop                 1378.80                          survived  kept: startup
#35   p1  ?             NestLoop                 1378.80                          REJECTED
-------------------------------------------------------------------
total events: 35

Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           BitmapHeapScan          0     0    1    0     1       57.15
  {1}           IndexScan               0     0    0    0     2        0.31

  {2}           BitmapHeapScan          0     0    1    0     0           -
  {2}           IndexScan               0     0    0    0     2       24.21
  {2}           SeqScan                 0     0    0    0     1     1042.00

  {1,2}         NestLoop                0    11    3    0     4     1149.67
  {1,2}         MergeJoin               0     5    0    0     0           -
  {1,2}         HashJoin                0     3    0    0     0           -
-------------------------------------------------------------------
```

## Gate view - proposed path: accepted, winner forbidden

```
PATHPROBE PROPOSE
----------------------------------------------------------------------
target : {1} (a)
mode   : winner_forbidden=on

CANDIDATE FATE
----------------------------------------------------------------------
pathname : cand1
provider : demo
type     : CustomScan
cost     : (0.10..3.00)
rows     : 1

fate     : survived (reached upper rel)
winner   : IndexScan @ 8.30
gap      : -5.30

CONTEXT
----------------------------------------------------------------------
raw best         : cand1 @ 3.00
effective winner : IndexScan @ 8.30

SEARCH CONTEXT (summary)
----------------------------------------------------------------------
Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     0           -
  {1}           IndexScan               0     0    0    1     1        8.30
  {1}           BitmapHeapScan          0     0    1    0     0           -
  {1}           CustomScan              0     0    0    1     1        3.00
-------------------------------------------------------------------
```

## Gate view - proposed path: weak loser, policy off

```
PATHPROBE PROPOSE
----------------------------------------------------------------------
target : {1} (a)
mode   : winner_forbidden=off

CANDIDATE FATE
----------------------------------------------------------------------
pathname : weak1
provider : demo
type     : CustomScan
cost     : (100.00..9999.00)
rows     : 1

fate     : survived (reached upper rel)
winner   : IndexScan @ 8.30
gap      : +9990.70
note     : candidate won -- execution will fail in v0 (diagnostic mode only)

CONTEXT
----------------------------------------------------------------------
raw best         : weak1 @ 9999.00
effective winner : IndexScan @ 8.30

SEARCH CONTEXT (summary)
----------------------------------------------------------------------
Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     0           -
  {1}           IndexScan               0     0    0    1     1        8.30
  {1}           BitmapHeapScan          0     0    1    0     0           -
  {1}           CustomScan              0     0    0    1     1     9999.00
-------------------------------------------------------------------
```

## Gate view - multi-candidate, two relations

Candidate identity is the pathname; fate and winner are rel-local.

```
PATHPROBE PROPOSE
----------------------------------------------------------------------
target : {1} (doc)
mode   : winner_forbidden=on

CANDIDATE FATE
----------------------------------------------------------------------
pathname : doc_fast
provider : demo
type     : CustomScan
cost     : (0.00..1.00)
rows     : 40

fate     : survived
winner   : IndexScan @ 8.30
gap      : -7.30

CONTEXT
----------------------------------------------------------------------
raw best         : doc_fast @ 1.00
effective winner : IndexScan @ 8.30


PATHPROBE PROPOSE
----------------------------------------------------------------------
target : {2} (reg_buh)
mode   : winner_forbidden=on

CANDIDATE FATE
----------------------------------------------------------------------
pathname : reg_fast
provider : demo
type     : CustomScan
cost     : (0.00..2.00)
rows     : 60000

fate     : survived
winner   : BitmapHeapScan @ 29.79
gap      : -27.79

CONTEXT
----------------------------------------------------------------------
raw best         : reg_fast @ 2.00
effective winner : BitmapHeapScan @ 29.79

SEARCH CONTEXT (summary)
----------------------------------------------------------------------
Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     0           -
  {1}           IndexScan               0     0    0    1     1        8.30
  {1}           BitmapHeapScan          0     0    1    0     0           -
  {1}           CustomScan              0     0    0    1     1        1.00

  {2}           SeqScan                 0     0    0    1     0           -
  {2}           IndexScan               0     0    0    1     0           -
  {2}           BitmapHeapScan          0     0    0    1     1       29.79
  {2}           CustomScan              0     0    0    1     1        2.00

  {1,2}         NestLoop                0     5    2    1     2       38.16
-------------------------------------------------------------------
```

## Gate view - hypothetical index (JSON form)

The planner builds real IndexPaths over the fake index; they compete
through ordinary add_path().  Events are labeled `hypo:<name>`.

```
HYPOTHETICAL INDEXES
----------------------------------------------------------------------
physics: modeled (costs: am-native estimator; size: btree-shaped)
hypo:hx_id        on noidx(id)  injected: acc=2 rej=1 surv=1  best=8.05  IN PLAN
plan cost: 51.50 -> 8.05  (query-level benefit +43.45)

PathProbe  [probe0+probe1+precheck: full path life visible]
rels: 1=noidx  
-------------------------------------------------------------------
 seq  src  rel           pathtype              cost_total  gap                     event
-------------------------------------------------------------------
#1    p1  {1}           SeqScan                    51.50                          accepted
#2    p0  {1}           SeqScan                    51.50                          survived
#3    p1  {1}           IndexScan                   8.05  impr_by=+43.45          accepted hypo:hx_id
#4    p1  {1}           BitmapHeapScan              8.05                          REJECTED hypo:hx_id
#5    p1  {1}           IndexScan                   8.05                          survived [proposed] hypo:hx_id
#6    p1  ?             IndexScan                   8.05                          accepted hypo:hx_id
-------------------------------------------------------------------
total events: 6

Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     1       51.50
  {1}           IndexScan               0     0    0    1     1        8.05
  {1}           BitmapHeapScan          0     0    1    0     0           -
-------------------------------------------------------------------
```

## Gate view - hypothetical index (full CREATE INDEX grammar)

`hypo_sql` goes through the real parser: expressions, partial WHERE,
DESC/NULLS, opclass, INCLUDE (index-only scans work).  Costing uses the
AM's own amcostestimate, exactly as the optimizer does for real indexes.

```
HYPOTHETICAL INDEXES
----------------------------------------------------------------------
physics: modeled (costs: am-native estimator; size: btree-shaped)
hypo:h_inc        [CREATE INDEX h_inc ON noidx (id) INCLUDE (v)]  injected: acc=2 rej=1 surv=1  best=4.05  IN PLAN
plan cost: 51.50 -> 4.05  (query-level benefit +47.45)

PathProbe  [probe0+probe1+precheck: full path life visible]
rels: 1=noidx  
-------------------------------------------------------------------
 seq  src  rel           pathtype              cost_total  gap                     event
-------------------------------------------------------------------
#1    p1  {1}           SeqScan                    51.50                          accepted
#2    p0  {1}           SeqScan                    51.50                          survived
#3    p1  {1}           IndexOnlyScan               4.05  impr_by=+47.45          accepted hypo:h_inc
#4    p1  {1}           BitmapHeapScan              8.05                          REJECTED hypo:h_inc
#5    p1  {1}           IndexOnlyScan               4.05                          survived [proposed] hypo:h_inc
#6    p1  ?             IndexOnlyScan               4.05                          accepted hypo:h_inc
-------------------------------------------------------------------
total events: 6

Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     1       51.50
  {1}           IndexOnlyScan           0     0    0    1     1        4.05
  {1}           BitmapHeapScan          0     0    1    0     0           -
-------------------------------------------------------------------
```

## Gate view - candidate pathkeys (output-order claim)

Same candidate cost, same `ORDER BY id` query.  Without pathkeys the
candidate only survives the base frontier (a Sort would sit on top);
claiming `"planner": {"pathkeys": ["id"]}` the CustomPath reaches the
upper rel as-is - visible directly in the fate:

Without pathkeys:

```
PATHPROBE PROPOSE
----------------------------------------------------------------------
target : {1} (noidx)
mode   : winner_forbidden=on

CANDIDATE FATE
----------------------------------------------------------------------
pathname : c_plain
provider : demo
type     : CustomScan
cost     : (0.50..5.00)
rows     : 1100

fate     : survived
winner   : SeqScan @ 51.50
gap      : -46.50

CONTEXT
----------------------------------------------------------------------
raw best         : c_plain @ 5.00
effective winner : SeqScan @ 51.50

SEARCH CONTEXT (summary)
----------------------------------------------------------------------
Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     1       51.50
  {1}           CustomScan              0     0    0    1     1        5.00
-------------------------------------------------------------------
```

With `"pathkeys": ["id"]`:

```
PATHPROBE PROPOSE
----------------------------------------------------------------------
target : {1} (noidx)
mode   : winner_forbidden=on

CANDIDATE FATE
----------------------------------------------------------------------
pathname : c_sorted
provider : demo
type     : CustomScan
cost     : (0.50..5.00)
rows     : 1100

fate     : survived (reached upper rel)
winner   : SeqScan @ 51.50
gap      : -46.50

CONTEXT
----------------------------------------------------------------------
raw best         : c_sorted @ 5.00
effective winner : SeqScan @ 51.50

SEARCH CONTEXT (summary)
----------------------------------------------------------------------
Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     1       51.50
  {1}           CustomScan              0     0    0    1     1        5.00
-------------------------------------------------------------------
```

## Gate view - advisor mode

`{"advise": true}` derives candidates from the relation's restrictions
(catalog capability test: operator strategy in the column's default btree
opclass family) and query/group pathkeys.  `IN PLAN` marks candidates whose
fake index reached the final plan; the closing line is the query-level
benefit against a baseline planning run without injections.

```
HYPOTHETICAL INDEXES
----------------------------------------------------------------------
atoms  : noidx: id=, v~   (=eq, ~range)
physics: modeled (costs: am-native estimator; size: btree-shaped)
hypo:adv_1        [CREATE INDEX adv_1 ON noidx (id)]  injected: acc=1 rej=1 surv=1  best=8.05
hypo:adv_2        [CREATE INDEX adv_2 ON noidx (v)]  injected: acc=0 rej=2 surv=0  best=8.05
hypo:adv_3        [CREATE INDEX adv_3 ON noidx (id, v)]  injected: acc=2 rej=1 surv=1  best=4.05  IN PLAN
plan cost: 59.00 -> 4.05  (query-level benefit +54.95)

PathProbe  [probe0+probe1+precheck: full path life visible]
rels: 1=noidx  
-------------------------------------------------------------------
 seq  src  rel           pathtype              cost_total  gap                     event
-------------------------------------------------------------------
#1    p1  {1}           SeqScan                    59.00                          accepted
#2    p0  {1}           SeqScan                    59.00                          survived
#3    p1  {1}           IndexScan                   8.05  impr_by=+50.95          accepted hypo:adv_1
#4    p1  {1}           BitmapHeapScan              8.05                          REJECTED hypo:adv_1
#5    p1  {1}           IndexScan                   8.05                          survived [proposed] hypo:adv_1
#6    p1  {1}           IndexScan                   8.05                          REJECTED hypo:adv_2
#7    p1  {1}           BitmapHeapScan              8.05                          REJECTED hypo:adv_2
#8    p1  {1}           IndexOnlyScan               4.05  impr_by=+4.00           accepted hypo:adv_3
#9    p1  {1}           BitmapHeapScan              8.06                          REJECTED hypo:adv_3
#10   p1  {1}           IndexOnlyScan               4.05                          survived [proposed] hypo:adv_3
#11   p1  ?             IndexOnlyScan               4.05                          accepted hypo:adv_3
-------------------------------------------------------------------
total events: 11

Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     1       59.00
  {1}           IndexScan               0     0    1    1     1        8.05
  {1}           BitmapHeapScan          0     0    3    0     0           -
  {1}           IndexOnlyScan           0     0    0    1     1        4.05
-------------------------------------------------------------------
```

## Gate view - hints (constrained counterfactual)

`hints.forbid` / `hints.allow_only` scope enable_* GUCs to this planning
run only (session state restored).  Here the true winner (IndexScan @ 8.30)
is forbidden and SeqScan becomes the effective winner:

```
PATHPROBE PROPOSE
----------------------------------------------------------------------
target : {1} (a)
mode   : winner_forbidden=on

CANDIDATE FATE
----------------------------------------------------------------------
pathname : cand1
provider : demo
type     : CustomScan
cost     : (0.00..99999.00)
rows     : 1

fate     : survived (reached upper rel)
winner   : SeqScan @ 99.50
gap      : +99899.50

CONTEXT
----------------------------------------------------------------------
raw best         : cand1 @ 99999.00
effective winner : SeqScan @ 99.50

SEARCH CONTEXT (summary)
----------------------------------------------------------------------
Summary (per relation x strategy)
-------------------------------------------------------------------
  rel           strategy             excl  skip  rej  acc  surv  best_cost
-------------------------------------------------------------------
  {1}           SeqScan                 0     0    0    1     1       99.50
  {1}           IndexScan               0     0    1    0     0           -
  {1}           BitmapHeapScan          0     0    1    0     0           -
  {1}           CustomScan              0     0    0    1     1    99999.00
-------------------------------------------------------------------
```

## Inline EXPLAIN (TEXT)

```
QUERY PLAN                                         
--------------------------------------------------------------------------------------------
 Index Scan using a_id on a
   Index Cond: (id = 42)
 
 PathProbe  [probe0+probe1+precheck: full path life visible]
 rels: 1=a  
 -------------------------------------------------------------------
  seq  src  rel           pathtype              cost_total  gap                     event
 -------------------------------------------------------------------
 #1    p1  {1}           BitmapHeapScan                                            REJECTED
 #2    p0  {1}           IndexScan                                                 survived
 -------------------------------------------------------------------
 total events: 2
 
 Summary (per relation x strategy)
 -------------------------------------------------------------------
   rel           strategy             excl  skip  rej  acc  surv  best_cost
 -------------------------------------------------------------------
   {1}           BitmapHeapScan          0     0    1    0     0           -
   {1}           IndexScan               0     0    0    0     1           -
 -------------------------------------------------------------------
(20 rows)
```

## Inline EXPLAIN (FORMAT JSON, machine-readable)

```
[
  {
    "Plan": {
      "Node Type": "Index Scan",
      "Parallel Aware": false,
      "Async Capable": false,
      "Scan Direction": "Forward",
      "Index Name": "a_id",
      "Relation Name": "a",
      "Alias": "a",
      "Disabled": false,
      "Index Cond": "(id = 42)"
    },
    "PathProbe": [
      {
        "Seq": 1,
        "Rel": "{1}",
        "Path Type": "BitmapHeapScan",
        "Event": "rejected",
        "Parameterized": false
      },
      {
        "Seq": 2,
        "Rel": "{1}",
        "Path Type": "IndexScan",
        "Event": "survived",
        "Parameterized": false
      }
    ]
  }
]
```

## Gate symbols (canonical)

| symbol | meaning |
|---|---|
| `p0` | probe0: survivor snapshot of a base rel (set_rel_pathlist) |
| `p1` | probe1: add_path verdicts - REJECTED / accepted (core patch) |
| `pc` | precheck: killed before a Path was built (SKIPPED) |
| `j2` | probe2: join-rel survivor snapshot (set_join_pathlist) |
| `wp` | proposed candidate event (retagged) |

## Gate columns (canonical)

| column | meaning |
|---|---|
| `seq` | global event order |
| `src` | gate symbol above |
| `rel` | relids set, `?` = upper rel (relids empty) |
| `pathtype` | path node type |
| `cost_total` | total cost (blank under COSTS OFF) |
| `gap` | `lost_by`/`impr_by` when known; `dominated_early` for precheck |
| `event` | excluded / SKIPPED / REJECTED / accepted / survived / displaced |
| label | `[param]`, `[outer]`, `[proposed]`, `disabled` (forbidden by hints), `kept: startup`, `kept: order?`, `hypo:<name>`, candidate pathname |

## Proposed candidate card (canonical minimum)

pathname, target (rel-local), mode, fate
(for example `survived` / `accepted -> displaced` / `rejected` / `target not found`),
effective winner with cost, gap, note when policy prevented a raw-cost win.
Hypothetical indexes get a per-index line:
`hypo:<name> [...]  injected: acc=N rej=N surv=N  best=C`.
