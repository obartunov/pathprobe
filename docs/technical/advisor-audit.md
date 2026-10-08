# Advisor audit

The current advisor is a local candidate generator.  PathProbe measures what
happened to the generated candidates.  It does not claim that a candidate is
globally good for the workload.

This audit records what the advisor (`pathprobe_advisor.c`) generates for
common query patterns, what it skips, where its output is misleading, and
what it does not explain.  Evidence: `examples/09_advisor_audit.sql` (one
table of 30000 rows, existing indexes on `(b, a)` and `(lower(d))`, a second
table of 3000 rows), plus reading the code.  Nothing here changes behaviour.

## How it works (from the code)

Per base relation, during planning, when `{"advise": true}`:

```text
atoms       equality members of const-bearing equivalence classes, then
            baserestrictinfo: col op const with a strategy in the column's
            default btree opfamily, col = ANY(array), col IS NULL, boolean
            forms (see "Boolean quals" below), and the same with an
            expression instead of a column; at most 6 atoms per relation
            (PATHPROBE_ADVISE_MAX); atoms beyond it are dropped and, after
            the fix, counted (advisor.atoms_dropped, see below)
single      one candidate per plain-column atom, equalities first
composite   all equality columns + the first range column, if >= 2 columns
expression  one candidate per expression atom
covering    composite (or the first atom) + INCLUDE of the relation's other
            needed columns, if they fit in 8 columns
ordered     query_pathkeys, else group_pathkeys, ASC only, all from this
            relation
dedup       same column list as an earlier candidate of this run; same keys
            as an existing plain btree index; with advise_skip_prefix also a
            strict prefix of an existing plain btree index; for expression
            candidates the same against existing expression indexes (after
            the fix of bug 3, see below); partial indexes never count
caps        pathprobe.max_advisor_candidates per query (advisor.truncated);
            the atom limit is reported apart (advisor.atom_truncated)
```

Every candidate becomes a hypothetical index named `adv_N` and goes through
the same path as `hypo_indexes`: injection, `build_index_paths()`, `add_path()`.

## Patterns

Fates are from the example run at the time of the audit.  `built` =
`index_build.generated > 0`.  Bug 1 was fixed after the audit (see below);
since then the four rows that won only by it are `rejected` with `in_plan`
false: the covering variant of `ex9_u`'s primary key, `(lower(d))`, `(a, b)`
next to `(b, a)`, and the six-column composite.  Bug 2 was fixed after the
audit too: both OR rows now produce no atom and no candidate.  After the
fix of bug 3 the `lower(d)` row produces no candidate at all.  The
eight-restriction row keeps the same six atoms and candidates, but the two
dropped atoms are now reported.

```text
query pattern              advisor behavior                       candidate?  hypo fate             in_plan  class
-------------------------  -------------------------------------  ----------  --------------------  -------  -------------------------
c = 3                      single (c) + covering                  yes         (c) survived;         (c)      handled
                                                                              covering rejected
ts BETWEEN ...             single (ts) + covering                 yes         (ts) survived         (ts)     handled
c = 3 AND ts > ...         singles, composite (c, ts), covering   yes         composite survived,   (c, ts)  handled
                                                                              singles displaced
ORDER BY ts LIMIT 10       ordered (ts), no atoms                 yes         survived              yes      handled
ORDER BY ts DESC LIMIT 10  nothing                                no          -                     -        intentional limitation;
                                                                                                             should be documented
GROUP BY c                 ordered (c) from group_pathkeys        yes         survived              no       handled
join t.c = u.w, u.id = 3   no atom for t; covering on u's PK      no for t    covering survived     yes      intentional limitation;
                                                                                                             covering wins by bug 1
join t.c = u.w, u.w = 3    t.c = 3 by equivalence: (c) on t       yes         survived              yes      handled (transitivity)
c = 3 OR ts = ...          whole OR as one boolean expression     yes         rejected              no       bug (docs say OR skipped)
ts = x OR ts = y           whole OR as one boolean expression;    yes         rejected              no       bug; misleading
                           no plain (ts) candidate
payload IS NULL            single (payload) + covering            yes         survived              yes      handled
payload IS NOT NULL        skipped (code: "rarely selective")     no          -                     -        intentional limitation
(a + b) = 77               expression ((a + b))                   yes         survived              yes      handled
lower(d) = ..., index      expression ((lower(d))) again          yes         survived              yes      bug: duplicate of an
  on lower(d) exists                                                                                         existing index
flag AND c = 3             (c), (flag), (c, flag), covering       yes         (c, flag) survived    yes      handled; no partial
                                                                                                             index ever proposed
a = 7 AND b = 7,           (a), (b), (a, b), covering             yes         (a, b) survived       yes      unclear: functional
  index (b, a) exists                                                                                        duplicate not deduped
b = 7, index (b, a)        (b) + covering; (b) dropped with       yes         rejected              no       handled (documented
  exists                   advise_skip_prefix                                                                option)
d LIKE 'v1%'               no atom                                no          -                     -        limitation; should be
                                                                                                             documented
8 restrictions             6 atoms kept (flag, payload dropped)   yes         composite survived    yes      unclear: silent cap
```

## Boolean quals

The policy after the fix of bug 2 (`adv_bool_operand()`), per top-level
restriction:

```text
clause                                atom
------------------------------------  ----------------------------------------
flag (boolean column)                 yes: (flag), equality-like
NOT flag                              yes: (flag)
flag IS TRUE / IS FALSE               yes: (flag)
f(x) returning boolean (isfinite(ts)) yes: expression atom ((f(x))); an index
                                      on f(x) serves WHERE f(x)
NOT f(x), f(x) IS TRUE / IS FALSE     yes: ((f(x)))
x = 1 OR y = 2, x = 1 OR x = 2        no: skipped as a whole (no per-branch
                                      atom, no common-column extraction)
(a = 1 AND b = 2) OR c = 3            no: skipped
a = 1 AND b = 2 (top level)           not one clause: the planner splits it,
                                      and each child is an atom (a=, b=)
(a > b) IS TRUE, other boolean        no: not proposed as an expression index
  expressions (CASE, IS DISTINCT ...)
col op const returning boolean        handled as an operator qual, not as a
                                      boolean expression
other BooleanTest forms (IS UNKNOWN)  no, as in core
```

## Traceability

Each candidate traces cleanly through the output, by name:

```text
advisor candidate   hypo_indexes[].name = adv_N, .ddl
-> hypo index       .injected
-> index build      .index_build.generated / .not_generated
-> add_path()       .accepted / .rejected / .displaced / .survived, .fate
-> final plan       .in_plan
```

In every pattern above each candidate built index paths (no
`not_generated`): the advisor only proposes keys taken from indexable
clauses or pathkeys.  What does not trace is the step before: which clause or
rule produced a candidate.  The link is the naming convention and the
`atoms` display string.

## Useless or duplicate candidates

```text
same keys as an existing plain btree index      skipped (always)
same keys as an existing expression index       NOT skipped: re-proposed (bug 3; now
                                                skipped, fixed after the audit)
same keys, different order (a, b) vs (b, a)     not skipped; for equality-only
                                                queries a functional duplicate
strict prefix of an existing index              proposed unless advise_skip_prefix
candidate with no generated IndexPath           none observed
candidate always rejected                       covering INCLUDE candidates: rejected
                                                in every pattern here (tables never
                                                vacuumed, so no index-only scan
                                                advantage), except the covering
                                                variant of ex9_u's primary key,
                                                which wins only by bug 1; the OR
                                                expression candidates
displaced by another advisor candidate          single-column candidates by the
                                                composite: expected
```

## Bugs found

1. **Hypothetical btree indexes are costed without descent cost.**  Both
   index builders set `tree_height = -1` with the comment "let
   btcostestimate derive", but `btcostestimate()` uses `tree_height`
   directly (`selfuncs.c`: `(tree_height + 1) * 50 * cpu_operator_cost`);
   only the gist and spgist estimators derive an unknown height.  A
   hypothetical btree index is therefore cheaper than an identical real one
   by the real index's descent cost.  Observed: every functional duplicate
   (`(lower(d))`, `(a, b)` next to `(b, a)`, the six-column composite next to
   the primary key, a covering variant of a primary key) wins with a
   `query_level_benefit` of exactly 0.25 and `in_plan = true`.  This makes
   duplicates look useful.

   Fixed after the audit: both builders estimate the height of a freshly
   built btree from the leaf page estimate, with internal pages filled to
   `BTREE_NONLEAF_FILLFACTOR`; other AMs keep -1.  A hypothetical btree
   over the same keys as a real one now has the same startup and total
   cost (one- and two-level trees checked, `pathprobe_hypo_cost`).  The
   duplicates above are no longer in the plan and report no benefit.  For
   an expression without statistics the width falls back to the type's
   average, so its page count, and possibly its height, can exceed the real
   index's: the hypothetical index then costs more, not less.
2. **An OR restriction becomes a boolean expression candidate.**  The
   "bare boolean clause" branch of `adv_col_btree_qual()` accepts any
   boolean-typed clause, including an OR `BoolExpr`, so the advisor proposes
   `CREATE INDEX ... (((c = 3) OR (ts = ...)))`.  `docs/user/advisor.md`
   said OR branches are skipped.  For `ts = x OR ts = y` no `(ts)` candidate
   is generated.

   Fixed after the audit: the operand of a boolean qual must be a boolean
   column or a function returning boolean, so an OR (and any other
   boolean expression) produces no atom.  An OR is skipped as a whole,
   also on one column: `ts = x OR ts = y` gets no `(ts)` candidate.  The
   final policy is under "Boolean quals".
3. **Expression candidates bypass deduplication.**  They are not checked by
   `adv_skip_candidate()`, and `adv_coverage()` ignores expression indexes,
   so an existing expression index is proposed again.

   Fixed after the audit: an expression candidate is checked against the
   relation's real btree indexes with the planner's own test,
   `match_index_to_operand()` (`equal()` on the expression trees, with
   RelabelType stripped), on each index's leading key.  An index whose only
   key matches is a duplicate and the candidate is always skipped; an index
   with more keys and the expression first, such as `(lower(d), a)`, skips it
   only with `advise_skip_prefix`, as for column candidates.  Partial,
   hypothetical and non-btree indexes do not count, as for column candidates;
   the index's operator class is not compared, as for column candidates.

## Unclear or undocumented behavior

- At most 6 atoms per relation; further restrictions are dropped without a
  flag (`advisor.truncated` is only the candidate cap).

  Fixed after the audit, as far as visibility goes: the limit and the
  candidates are unchanged, but dropped atoms are counted.  The advisor
  block has `atom_limit`, `atoms_seen`, `atoms_kept`, `atoms_dropped` and
  `atom_truncated` (summed over relations; a distinct atom is counted once),
  and the `atoms` string ends a relation's list with `(+N dropped)`.
  `truncated` keeps its meaning, the candidate cap.  Which atoms are kept is
  still decided by extraction order (equivalence-class equalities first),
  not by usefulness, and the dropped atoms are not named.
- `ORDER BY ... DESC` (and `NULLS FIRST`) produce no ordered candidate,
  although an ASC btree serves a uniform DESC order by a backward scan.
- `LIKE 'prefix%'` produces no atom: `~~` has no strategy in the default
  btree opfamily; `text_pattern_ops` or a C collation are not considered.
- Key order is part of the dedup signature, so `(a, b)` is proposed next to
  an existing `(b, a)`.
- A join clause alone never produces an atom, but a constant that reaches a
  join column through an equivalence class does.
- No partial index is ever proposed.

## What the output does not explain

```text
why a candidate was generated   single / composite / expression / covering /
                                ordered: not reported
which clause(s) caused it       only via the atoms string and the DDL
why a clause was skipped        not reported (join qual, IS NOT NULL, LIKE,
                                DESC); the atom cap only as a count
                                (advisor.atoms_dropped), not which atoms
whether it duplicates or        not reported; existing indexes are not named
  extends an existing index
advisor candidate -> hypo link  adv_N naming only; no source field
benefit per candidate           query_level_benefit is for the whole set;
                                in_plan shows which candidate is used
```

## Follow-ups

Bugs first.  Items marked done were implemented after the audit.

1. Done: hypothetical btree indexes get a tree height, so they are not
   cheaper than identical real indexes.
2. Done: OR / AND `BoolExpr` is not a boolean atom.
3. Done: expression candidates are deduplicated against existing
   expression indexes.
4. Done in part: dropped atoms (cap) are counted in the advisor block.
   Still open: skipped clause classes in the advisor block; mark each
   hypothetical index with its source and generation rule.  These are
   additive JSON fields and need a gate.
