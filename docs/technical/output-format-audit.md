# Output format audit

An audit of how readable pathprobe's output is, based on the eight runnable
examples in `docs/examples/`.  It records where a human or a tool has to dig,
and classifies possible improvements.  Nothing here changes the format.

The question: can a reader follow this chain from the output?

```text
advisor proposed candidate
  -> injected
  -> examined by build_index_paths()
  -> IndexPath generated, or not_generated
  -> accepted / rejected / displaced / survived
  -> in_plan
  -> coverage: what was actually observed
```

Answer: yes from JSON, with work.  Every link exists as a field, but no single
place shows the chain; a reader joins `advisor`, `hypo_indexes[]`,
`index_build`, the event counts, `coverage` and sometimes `events[]`.  The
examples do this with small SQL helpers (examples 4, 5, 7) and the jq recipes
below.  The text form shows most of it on one line per hypothetical index,
but not displacement counts or the build decision.

## Per example

### 1. EXPLAIN is not enough

- Easy: per-strategy counts at the join rel; `excluded` for a disabled join
  family.
- Digging: the summary has no displaced column; displacement needs
  `events[]`.  Two `survived` events for one hash join are pair-local `j2`
  snapshots, not two paths; nothing in the event says so.
- Defect found: a real survivor kept for its ordering carries
  `"label": "  kept: order?"` and therefore `"origin": "proposed"` in JSON
  (`pathprobe.c` and `pathprobe_probe2.c` reuse the label field for the text
  annotation).  A tool that filters by `origin` or `label` miscounts.

### 2. Displaced paths

- Easy: `candidates[].fate = displaced` with `event_counts`.
- Who displaced whom was implicit (the ACCEPTED event right before, same
  `gap_cost`).  Now explicit: DISPLACED events carry `displacer_path_id`,
  `displacer_seq` and `other_pathtype`, and every path has a `path_id`
  (see `json-format.md`, "Path identity and graph edges").
- Confusing: `event_counts.survived = 1` next to `fate = displaced` (the
  frontier snapshot after injection is counted).  `survived_upper` vs
  `survived` needs the docs.
- Text card: `winner` is the cheapest real path in the relation's snapshot
  taken before injection, not the plan winner.  The `note: candidate won --
  execution will fail` line is derived from any SURVIVED event of the
  candidate, including the snapshot right after its own `add_path()`, so the
  displaced `early` is also reported as having won.

### 3. target_not_found / not_examined / unknown

- Easy: the three fates are distinct values.
- Implicit: why.  `target_in_query` is not emitted; `injected = false` is the
  same for both of the first two.  `unknown` is explained only by reading
  `coverage` (here `add_path_rejected = false`).

### 4. Not generated index path

- Easy: `fate = not_generated` and `index_build.not_generated` reasons.
- Confusing: `index_build.generated` counts build calls that returned paths,
  not paths (`ab` shows `generated = 1` and two rejected paths).
  `not_generated` is a union over all calls; which call site gave which
  reason is lost.
- Two places say "why": `fate_reason` (for `not_examined` and `unknown`)
  and `index_build.not_generated` (for `not_generated`).

### 5. Hypothetical index lifecycle

- Easy: one fate per index, `in_plan` separate.
- Confusing: counts include upper-rel events, fate does not (`on_a` has
  `accepted = 3`).  `survived` count is the post-injection snapshot, not the
  final pathlist (`narrow`: `survived = 1`, fate `displaced`).
- Implicit: `unknown(outcome_not_observed)` for OR-arm paths requires
  knowing that BitmapOr arms carry no label.
- Hypothetical index events have `"origin": "proposed"`; they are ordinary
  index paths over a hypothetical index.

### 6. Agent preflight

- Easy: `coverage`, `limits`, `excluded` events.
- Digging: "only a sequential scan survived" needs a self-join over
  `summary`; relation sizes are not in the output.  `rels` maps range-table
  indexes to aliases, not relation names.
- `pathprobe_json()` never covers `index_build`, so a preflight check always
  sees that gap unless it uses the propose functions.

### 7. Advisor candidate lifecycle

- Easy: the text line per candidate (`hypo:adv_3 [...] injected: acc=2
  rej=0 surv=1 best=15.18 IN PLAN fate=survived`).
- Digging: the text line omits the displaced count and the build decision;
  JSON has both.
- Implicit: the link from `advisor` to its candidates is the `adv_N` naming
  convention; `advisor.atoms` is a display string, and nothing says which
  atom produced which candidate (the `created` candidate comes from
  `ORDER BY`, not from atoms).
- `query_level_benefit` belongs to the candidate set; only `in_plan` tells
  which candidate carries it.
- `rejected` for `adv_4` means "not better than an equal-cost path already
  kept", which depends on injection order.

### 8. Advisor limitations

- Easy: `atoms` shows what the advisor extracted.
- Silent: skipped clauses (join quals, `IS NOT NULL`) are not reported; an
  empty atom list for a relation (`"ex8_orders:  |"`) is the only trace.
- Confusing: an `advisor` block with `enabled = false` is emitted for a
  hypothetical-index-only call and carries that call's benefit.
- Legacy fields: hypothetical-index-only and advisor-only calls still emit
  `"target": {"relname": ""}` and a `"candidate"` object with empty strings;
  only the legacy top-level `fate` is omitted.

## Focus points

```text
top-level fate vs candidates[].fate  same derivation; top-level is
                                     candidates[0] only and absent without
                                     an explicit candidate; legacy target /
                                     candidate blocks are still emitted empty
hypo fate / fate_reason              clear values; "why" split between
                                     fate_reason and index_build.not_generated
index_build.generated / not_generated
                                     counts calls, not paths; reasons are a
                                     union over call sites
coverage.*                           complete and correct, but nothing ties a
                                     particular unknown fate to the gap that
                                     caused it except fate_reason for hypos
displaced events                     present; displacer named by path_id and seq
rejected events                      dominator named by path_id and seq
advisor output                       no structured link to its candidates; no
                                     list of skipped clauses
in_plan                              clear; the only final-plan field
unknown / not_examined / not_generated
                                     distinct values; the reason for unknown
                                     is explicit for hypos (fate_reason), not
                                     for candidates
```

## Possible improvements

```text
#   improvement                                class                    decision
1   compact candidate lifecycle block          new helper function      later; SQL recipe now
2   compact hypo lifecycle block               new helper function      later; SQL recipe now (example 7)
3   one-line text summary per candidate/hypo   text-output only         partly exists; add disp=
                                                                        to the hypo line (follow-up)
4   rename survived_at_point / survived        requires semantic change do not do; documented
5   displacer type in JSON events              JSON additive field      done: other_pathtype,
                                                                        displacer_path_id/_seq;
                                                                        dominator_* for REJECTED
6   advisor -> candidate link                  JSON additive field      follow-up (e.g. hypo source
                                                                        "advisor" / "user")
7   jq recipes                                 docs-only                done, below
8   SQL helper / view / function               new helper function      do not do yet; SQL API
                                                                        addition needs a gate
```

Other findings, outside the list:

```text
A  "kept: ..." stored in label, making origin "proposed"  JSON fix (defect)     follow-up, first
B  empty legacy target/candidate blocks without an        JSON change           follow-up; omit like
   explicit candidate                                     (removal)             the top-level fate
C  advisor block with enabled=false carrying benefit      docs-only             documented (example 8)
D  hypothetical index events with origin "proposed"       docs-only             documented
E  index_build.generated counts calls                     docs-only             documented
F  counts include upper-rel events, fate does not         docs-only             documented
G  advisor skipped clauses not reported                   JSON additive field   later
H  text card "winner" and "note" lines                    text-output only      follow-up
```

## Proposed follow-ups

In order of value.  None is implemented here.

1. Keep `kept: startup` / `kept: order?` out of `label` (a separate event
   field or text-only rendering), so `origin` stays `real` for real paths.
   This is a correctness fix for tools that filter by origin or label.
2. Done: DISPLACED events name the displacer (`displacer_path_id`,
   `displacer_seq`, `other_pathtype`), REJECTED events the dominator
   (`dominator_path_id`, `dominator_seq`, `dominator_pathtype`; the core
   hook now passes the actual dominating path), every path event has a
   `path_id`, and `paths[]` has the child edges, including `child_path_ids`
   of Append, MergeAppend, BitmapAnd, BitmapOr, SetOp and RecursiveUnion.
3. Add the displaced count to the text line of a hypothetical index.
4. Omit the empty legacy `target` and `candidate` blocks when the call has no
   explicit candidate, as already done for the top-level `fate`.
5. Mark the source of each hypothetical index (`advisor` or `user`).

## jq recipes

Per hypothetical index lifecycle:

```sh
jq -r '.hypo_indexes[] | [.name, (.index_build.generated // "n/a"),
  .accepted, .rejected, .displaced,
  .fate + (if .fate_reason then "(" + .fate_reason + ")" else "" end),
  .in_plan] | @tsv'
```

Per candidate:

```sh
jq -r '.candidates[] | [.pathname, .fate, .event_counts.accepted,
  .event_counts.displaced] | @tsv'
```

Coverage gaps:

```sh
jq -r '.coverage | to_entries | map(select(.value == false) | .key) | join(",")'
```

Who displaced whom:

```sh
jq -r '.events[] | select(.kind == "displaced")
  | [.seq, .pathtype, .path_id, (.label // "-"),
     "by", .displacer_seq, .other_pathtype, .displacer_path_id] | @tsv'
```

On example 7:

```text
4   SeqScan         1  -           by  3   IndexScan       2
6   IndexScan       2  hypo:adv_1  by  5   BitmapHeapScan  3
11  BitmapHeapScan  3  hypo:adv_1  by  10  IndexScan       6
13  IndexScan       6  hypo:adv_3  by  12  BitmapHeapScan  7
```

Join paths and their children:

```sh
jq -r '.paths[] | select(.outer_path_id) |
  [.path_id, .pathtype, .rel, .outer_path_id, .inner_path_id] | @tsv'
```

Who rejected whom:

```sh
jq -r '.events[] | select(.kind == "rejected")
  | [.seq, .pathtype, .path_id, "by", .dominator_seq, .dominator_pathtype,
     .dominator_path_id] | @tsv'
```

Paths with a list of inputs:

```sh
jq -r '.paths[] | select(.child_path_ids) |
  [.path_id, .pathtype, .rel, (.child_path_ids | map(tostring) | join(","))]
  | @tsv'
```
