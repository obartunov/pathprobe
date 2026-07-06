#!/usr/bin/env python3
# pathprobe_report.py - part of pathprobe.
# Distributed under the PostgreSQL License; see the repository LICENSE file.
"""
pathprobe_report.py — summary-first report from pathprobe_json() output

Principle: collect exhaustively, present selectively.

QUICK START
-----------
  Ordinary pathprobe (observe real planner search):

    # Save to file, then report:
    psql -t -A -c "SET pathprobe.log_accepted=on; SELECT pathprobe_json('SELECT ...')" > out.json
    python pathprobe_report.py out.json

    # Pipe — no file needed:
    psql -t -A -c "SELECT pathprobe_json('SELECT ...')" | python pathprobe_report.py -

    # Pipe + gate matrix + rel filter (bash):
    psql -t -A -c "SELECT pathprobe_json('SELECT ...')" \
      | python pathprobe_report.py - --gate --rel "{1,2,4}"

    # Process substitution (bash):
    python pathprobe_report.py <(psql -t -A -c "SELECT pathprobe_json('SELECT ...')")

  Proposed path (inject candidate, observe fate):

    # Save to file, then report:
    psql -t -A -c "SELECT pathprobe_propose_json('SELECT ...', '{...}')" > propose.json
    python pathprobe_report.py propose.json

    # Pipe + gate matrix — one command (bash):
    psql -t -A -c "SELECT pathprobe_propose_json('SELECT ...', '{...}')" \
      | python pathprobe_report.py - --gate

PRESENTATION HIERARCHY
----------------------
  1. summary / hotspots  — where is search pressure?         (default)
  2. --gate (matrix)     — where exactly does each path die?  (middle layer)
  3. --profile           — per-rel strategy counts            (focused)
  4. raw trace           — forensic/debug only                (--rel/--kind/--src)

GATE MATRIX (--gate)
--------------------
  Canonical view: one row per strategy or candidate, one column per gate.

  Ordinary (strategy aggregate):
    NestLoop   40○  31✗   8✗   1✗   no survivors
    HashJoin   20○   7✗   2✗  10○   survived (10)

  Proposed (candidate lifecycle):
    wp:duckdb  ○  ○  ○  ✗  ⊘  forbidden ⊘  raw=18.00  winner=BitmapHeap@84.23
    wp:loser   ○  ○  ✗  ─  ─  rejected     raw=40.00  winner=BitmapHeap@84.23

  Gate columns: gen=generate  pre=precheck  cmp=compete  fr=frontier  pol=policy
  Symbols: ○ passed  ✗ first failing gate  ─ not reached  ⊘ policy blocked

COLUMN LEGEND (summary/profile tables)
---------------------------------------
  excl  = excluded before generation (pgs_mask blocked strategy)
  skip  = precheck: candidate never built
  rej   = add_path: built and lost competition
  acc   = add_path: entered frontier at some point
  disp  = accepted then displaced
  surv  = on final Pareto frontier  ◄ = winner
  pressure = (rej+skip+excl) / survived — how contested this joinrel was

SOURCE TAGS (--src filter)
--------------------------
  p0  = set_rel_pathlist_hook  (base rel frontier, no patch needed)
  p1  = add_path hook          (competition events, requires core patch)
  pc  = add_path_precheck hook (skipped paths, requires core patch)
  j2  = join-level hooks       (join frontier / excluded strategies)
  wp  = proposed path          (from pathprobe_propose_json())

EXAMPLES
--------
  # Default: summary + top 5 hotspots + profiles
  python pathprobe_report.py out.json

  # Gate matrix for top-3 joinrels
  python pathprobe_report.py out.json --gate

  # Gate matrix for a specific joinrel
  python pathprobe_report.py out.json --gate --rel "{1,2,4,6}"

  # Gate matrix for proposed path
  python pathprobe_report.py propose.json --gate
  python pathprobe_report.py propose.json --gate --pathname duckdb_scan_filter

  # Winners only (join tree, no baserel noise)
  python pathprobe_report.py out.json --winner-only --no-baserel

  # Strategy profile for a specific joinrel
  python pathprobe_report.py out.json --profile "{1,2,4,6}"

  # Raw trace: NestLoop skipped events
  python pathprobe_report.py out.json --rel "{1,2,4,6}" --strategy NestLoop --kind skipped

  # Raw trace: all rejected, first 20
  python pathprobe_report.py out.json --kind rejected --limit 20

  # Raw trace: base-rel frontier survivors
  python pathprobe_report.py out.json --src p0

  # Raw trace: proposed path events only
  python pathprobe_report.py propose.json --src wp

  # Sort hotspots by elimination pressure
  python pathprobe_report.py out.json --sort pressure

  # Full raw trace, first 100 events
  python pathprobe_report.py out.json --raw-all --limit 100

TYPICAL WORKFLOW
----------------
  1. Default view to get orientation:
       python pathprobe_report.py out.json

  2. Gate matrix to see where paths die:
       python pathprobe_report.py out.json --gate --rel "{1,2,4}"

  3. Drill into a specific strategy:
       python pathprobe_report.py out.json --rel "{1,2,4}" --strategy NestLoop --kind skipped

  4. For proposed path experiments:
       python pathprobe_report.py propose.json          # candidate card + matrix
       python pathprobe_report.py propose.json --gate   # matrix only

  5. Winners map:
       python pathprobe_report.py out.json --winner-only --no-baserel
"""

import argparse
import json
import sys
from collections import defaultdict


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

KIND_ORDER = ["excluded", "skipped", "rejected", "accepted", "displaced", "survived"]

KIND_ABBREV = {
    "excluded":  "excl",
    "skipped":   "skip",
    "rejected":  "rej",
    "accepted":  "acc",
    "displaced": "disp",
    "survived":  "surv",
}

SRC_LABEL = {
    "pc": "precheck",
    "p1": "add_path",
    "p0": "rel_frontier",
    "j2": "join_hook",
    "wp": "proposed",
}


def load_json(path):
    if path == "-":
        raw = sys.stdin.read().strip()
    else:
        with open(path) as f:
            raw = f.read().strip()

    # Strip psql column header/footer if present
    lines = raw.splitlines()
    start = next((i for i, l in enumerate(lines) if l.strip().startswith("{")), None)
    if start is None:
        print("ERROR: no JSON object found in input", file=sys.stderr)
        sys.exit(1)
    end = next((i for i, l in enumerate(reversed(lines)) if l.strip() == "}"), None)
    if end is not None:
        end = len(lines) - end - 1
        raw = "\n".join(lines[start : end + 1])
    else:
        raw = "\n".join(lines[start:])

    # Strip leading spaces added by psql
    raw = "\n".join(l.lstrip(" +") for l in raw.splitlines())
    return json.loads(raw)


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

def build_rel_stats(data):
    """
    Per-rel aggregation.
    Returns dict: rel_key → {total, kinds: {kind: count}, strategies: {}}
    """
    rels = defaultdict(lambda: {
        "total": 0,
        "kinds": defaultdict(int),
        "strategies": defaultdict(lambda: defaultdict(int)),
        "best_cost": {},      # strategy → best survived cost
    })

    for ev in data["events"]:
        rel = ev["rel"]
        kind = ev["kind"]
        strategy = ev["pathtype"]
        rels[rel]["total"] += 1
        rels[rel]["kinds"][kind] += 1
        rels[rel]["strategies"][strategy][kind] += 1
        if kind == "survived":
            prev = rels[rel]["best_cost"].get(strategy)
            cost = ev.get("cost_total", 0)
            if prev is None or cost < prev:
                rels[rel]["best_cost"][strategy] = cost

    return rels


def classify_rel(rel_key):
    """Classify rel as baserel, joinrel, or upper."""
    if rel_key == "?":
        return "upper"
    if "{" in rel_key and "," in rel_key:
        return "joinrel"
    if "{" in rel_key:
        return "baserel"
    return "baserel"


def find_winner(rel_stats, rel_key):
    """Find surviving strategy with best cost for a rel."""
    best_cost = None
    best_strategy = None
    for strategy, cost in rel_stats[rel_key]["best_cost"].items():
        if best_cost is None or cost < best_cost:
            best_cost = cost
            best_strategy = strategy
    return best_strategy, best_cost


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def hr(width=70, char="─"):
    return char * width


def fmt_cost(cost):
    if cost is None:
        return "       —"
    return f"{cost:>10.2f}"


def print_global_summary(data, rel_stats, top_n, sort_key="events"):
    events = data["events"]
    rels_map = data.get("rels", {})

    total = len(events)
    kind_counts = defaultdict(int)
    for ev in events:
        kind_counts[ev["kind"]] += 1

    n_rels = len(rel_stats)
    n_baserels = sum(1 for r in rel_stats if classify_rel(r) == "baserel")
    n_joinrels = sum(1 for r in rel_stats if classify_rel(r) == "joinrel")

    print()
    print("PATHPROBE SUMMARY")
    print(hr())
    print(f"version      : {data.get('version', '?')}")
    if rels_map:
        rel_names = "  ".join(f"{k}={v}" for k, v in sorted(rels_map.items(), key=lambda x: int(x[0])))
        print(f"rels         : {rel_names}")
    print(f"total events : {total}")
    print(f"rel groups   : {n_rels}  ({n_baserels} baserel, {n_joinrels} joinrel)")
    print()

    # Event kind breakdown
    print("event breakdown:")
    for kind in KIND_ORDER:
        c = kind_counts.get(kind, 0)
        if c > 0:
            bar = "█" * min(40, c * 40 // max(kind_counts.values()))
            print(f"  {kind:<12} {c:>6}  {bar}")
    print()

    # Legend
    print("column legend:")
    print("  excl  = excluded before generation (pgs_mask)   "
          "skip  = precheck: never built")
    print("  rej   = add_path: built and lost               "
          "acc   = add_path: entered frontier")
    print("  disp  = accepted then displaced                  "
          "surv  = on final frontier  ◄ = winner")
    print("  pressure = (rej+skip+excl) / survived  — "
          "how contested this rel was")
    print()

    # Top hotspots
    sorted_rels = sorted(rel_stats.items(), key=lambda x: x[1]["total"], reverse=True)
    top = [(r, s) for r, s in sorted_rels if r != "?"][:top_n]

    if not top:
        return

    print(f"top {len(top)} hotspots (by event count):")
    print()
    sort_marker = f"  (sorted by {sort_key})"
    print(f"  {'rel':<18}  {'events':>7}  {'pressure':>9}  {'excl':>5}  {'skip':>5}  {'rej':>5}  {'acc':>5}  {'disp':>5}  {'surv':>5}  winner{sort_marker}")
    print(f"  {hr(105,'-')}")
    for rel_key, stats in top:
        k = stats["kinds"]
        surv = k.get('survived', 0)
        elim = k.get('rejected', 0) + k.get('skipped', 0) + k.get('excluded', 0)
        pressure = f"{elim/max(surv,1):.1f}x"
        winner_strategy, winner_cost = find_winner(rel_stats, rel_key)
        if winner_strategy and winner_cost is not None:
            winner_str = f"{winner_strategy} @ {winner_cost:.2f}"
        else:
            winner_str = "—"
        print(f"  {rel_key:<18}  {stats['total']:>7}  "
              f"{pressure:>9}  "
              f"{k.get('excluded',0):>5}  {k.get('skipped',0):>5}  "
              f"{k.get('rejected',0):>5}  {k.get('accepted',0):>5}  "
              f"{k.get('displaced',0):>5}  {k.get('survived',0):>5}  "
              f"{winner_str}")
    print()


def print_hotspot_profile(data, rel_stats, rel_key):
    """Per-hotspot strategy breakdown for a given rel."""
    if rel_key not in rel_stats:
        print(f"  rel '{rel_key}' not found in events")
        return

    stats = rel_stats[rel_key]
    winner_strategy, winner_cost = find_winner(rel_stats, rel_key)

    print(f"HOTSPOT  {rel_key}")
    print(hr())
    print(f"total events: {stats['total']}")
    if winner_strategy:
        print(f"winner      : {winner_strategy} @ {winner_cost:.2f}")
    print()

    # Strategy table sorted by total activity
    strategies = sorted(
        stats["strategies"].items(),
        key=lambda x: sum(x[1].values()),
        reverse=True
    )

    print(f"  {'strategy':<22}  {'excl':>5}  {'skip':>5}  {'rej':>5}  "
          f"{'acc':>5}  {'disp':>5}  {'surv':>5}  {'best_cost':>10}")
    print(f"  {hr(80,'-')}")
    for strategy, kinds in strategies:
        best = stats["best_cost"].get(strategy)
        is_winner = (strategy == winner_strategy)
        marker = " ◄" if is_winner else ""
        print(f"  {strategy:<22}  "
              f"{kinds.get('excluded',0):>5}  "
              f"{kinds.get('skipped',0):>5}  "
              f"{kinds.get('rejected',0):>5}  "
              f"{kinds.get('accepted',0):>5}  "
              f"{kinds.get('displaced',0):>5}  "
              f"{kinds.get('survived',0):>5}  "
              f"{fmt_cost(best)}{marker}")
    print()


def print_top_hotspot_profiles(data, rel_stats, top_n, sort_key="events"):
    """Print strategy profiles for top-N hotspots."""
    sorted_rels = sorted(rel_stats.items(), key=lambda x: x[1]["total"], reverse=True)
    top = [(r, s) for r, s in sorted_rels if r != "?"][:top_n]
    for rel_key, _ in top:
        print_hotspot_profile(data, rel_stats, rel_key)


def print_raw_trace(data, rel_filter=None, strategy_filter=None,
                    kind_filter=None, src_filter=None, limit=None):
    """Print filtered raw event trace."""
    events = data["events"]

    filtered = []
    for ev in events:
        if rel_filter and ev["rel"] != rel_filter:
            continue
        if strategy_filter and ev["pathtype"] != strategy_filter:
            continue
        if kind_filter and ev["kind"] != kind_filter:
            continue
        if src_filter and ev["src"] != src_filter:
            continue
        filtered.append(ev)

    total = len(filtered)
    shown = filtered[:limit] if limit else filtered

    filter_desc = []
    if rel_filter:      filter_desc.append(f"rel={rel_filter}")
    if strategy_filter: filter_desc.append(f"strategy={strategy_filter}")
    if kind_filter:     filter_desc.append(f"kind={kind_filter}")
    if src_filter:      filter_desc.append(f"src={src_filter}")
    filter_str = "  ".join(filter_desc) if filter_desc else "no filter"

    print(f"RAW TRACE  [{filter_str}]")
    print(hr())
    print(f"matching events: {total}" + (f"  (showing first {limit})" if limit and total > limit else ""))
    print()

    if not shown:
        print("  (no events match filter)")
        return

    print(f"  {'seq':>5}  {'src':<4}  {'rel':<18}  {'pathtype':<22}  "
          f"{'cost':>10}  {'gap':>12}  kind")
    print(f"  {hr(85,'-')}")

    for ev in shown:
        seq  = ev["seq"]
        src  = ev["src"]
        rel  = ev["rel"]
        pt   = ev["pathtype"]
        cost = ev.get("cost_total", 0)
        kind = ev["kind"]

        gap_str = ""
        gap = ev.get("gap_cost")
        if gap is not None:
            label = "lost_by" if kind == "rejected" else "impr_by"
            gap_str = f"{label}={gap:+.2f}"

        skip_reason = ev.get("skip_reason", "")
        if skip_reason:
            gap_str = skip_reason

        param_mark = " [param]" if ev.get("param") else ""
        kind_display = kind.upper() if kind in ("rejected", "skipped", "excluded") else kind

        print(f"  {seq:>5}  {src:<4}  {rel:<18}  {pt:<22}  "
              f"{cost:>10.2f}  {gap_str:>12}  {kind_display}{param_mark}")

    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def print_winners(data, rel_stats, no_baserel=False):
    """Show final winner per rel — one line each, cheapest surviving path."""
    rels_map = data.get("rels", {})

    # Build winner per rel from events (cheapest survived per rel)
    winners = {}   # rel → (strategy, cost, src)
    for ev in data["events"]:
        if ev["kind"] != "survived":
            continue
        rel = ev["rel"]
        cost = ev.get("cost_total", 0)
        if rel not in winners or cost < winners[rel][1]:
            winners[rel] = (ev["pathtype"], cost, ev["src"])

    # Sort: baserels first, then joinrels by size, upper last
    def rel_sort_key(item):
        r = item[0]
        if r == "?":
            return (3, 0, r)
        if "{" not in r:
            return (0, 0, r)
        n = r.count(",") + 1
        return (1 if n > 1 else 0, n, r)

    if no_baserel:
        winners = {r: v for r, v in winners.items()
                   if r != "?" and "{" in r and "," in r}
    sorted_w = sorted(winners.items(), key=rel_sort_key)

    scope = "join tree only" if no_baserel else "all rels"
    print(f"WINNERS  (cheapest survived path per rel — {scope})")
    print(hr())
    print(f"  {'rel':<22}  {'strategy':<22}  {'cost':>10}  src")
    print(f"  {hr(62,'-')}")
    for rel, (strategy, cost, src) in sorted_w:
        # resolve rel name if baserel
        if "{" not in rel and rel != "?":
            display = rel
        else:
            display = rel
        print(f"  {display:<22}  {strategy:<22}  {cost:>10.2f}  {src}")
    print()


def print_candidate_card(data):
    """Candidate-first view for pathprobe_propose_json() output."""
    cand  = data.get("candidate", {})
    tgt   = data.get("target", {})
    fate  = data.get("fate", {})
    rels  = data.get("rels", {})

    # resolve rel id to name
    rel_name = tgt.get("relname", "?")
    rel_id   = next((k for k, v in rels.items() if v == rel_name), "?")
    rel_label = f"{{{rel_id}}}" if rel_id != "?" else "?"

    wf = "winner_forbidden=on" if cand.get("winner_forbidden") else "winner_forbidden=off"

    print()
    print("PATHPROBE PROPOSE")
    print(hr())
    print(f"target : {rel_label} ({rel_name})")
    print(f"mode   : {wf}")
    print()

    # Candidate fate block
    print("CANDIDATE FATE")
    print(hr())
    print(f"pathname : {cand.get('pathname', '?')}")
    print(f"provider : {cand.get('provider', '?')}")
    print(f"type     : CustomScan")
    print(f"cost     : {cand.get('cost_total', 0):.2f}")
    print(f"rows     : {cand.get('rows', 0):.0f}")
    print()

    fate_str = fate.get("fate", "unknown").replace("_", " -> ").replace("not -> survived", "not survived")
    print(f"fate     : {fate_str}")

    ws = fate.get("winner_strategy")
    wc = fate.get("winner_cost")
    if ws and wc is not None:
        print(f"winner   : {ws} @ {wc:.2f}")

    gap = fate.get("gap")
    if gap is not None:
        print(f"gap      : {gap:+.2f}")

    note = fate.get("note")
    if note:
        note_readable = note.replace("_", " ")
        print(f"note     : {note_readable}")
    print()

    # Context block
    print("CONTEXT")
    print(hr())
    if cand.get("cost_total") is not None:
        print(f"raw best         : {cand['pathname']} @ {cand['cost_total']:.2f}")
    if ws and wc is not None:
        print(f"effective winner : {ws} @ {wc:.2f}")
    print()

    # Search context — summary from events
    rel_stats = build_rel_stats(data)
    if rel_stats:
        print("SEARCH CONTEXT (summary)")
        print(hr())
        print(f"  {'strategy':<22}  {'excl':>5}  {'skip':>5}  {'rej':>5}  {'acc':>5}  {'surv':>5}  {'best_cost':>10}")
        print(f"  {hr(65,'-')}")
        # Show only target rel stats
        target_stats = rel_stats.get(rel_label) or next(
            (v for k, v in rel_stats.items() if "{" in k and "," not in k), None)
        if target_stats:
            strategies = sorted(target_stats["strategies"].items(),
                                key=lambda x: sum(x[1].values()), reverse=True)
            for strategy, kinds in strategies:
                best = target_stats["best_cost"].get(strategy)
                is_wp = (strategy == "CustomScan")
                marker = " ← wp" if is_wp else ""
                print(f"  {strategy:<22}  "
                      f"{kinds.get('excluded',0):>5}  "
                      f"{kinds.get('skipped',0):>5}  "
                      f"{kinds.get('rejected',0):>5}  "
                      f"{kinds.get('accepted',0):>5}  "
                      f"{kinds.get('survived',0):>5}  "
                      f"{fmt_cost(best)}{marker}")
        print()



# Gate symbols
G_PASS = "  ○  "   # passed this gate
G_DEAD = "  ✗  "   # died at this gate
G_SKIP = "  ─  "   # never reached (died earlier)
G_FORB = "  ⊘  "   # passed but forbidden by policy

GATES_ORDINARY  = ["generate", "precheck", "compete", "frontier"]
GATES_PROPOSED  = ["generate", "precheck", "compete", "frontier", "policy"]


def derive_gate_state(kinds, is_proposed=False, winner_forbidden=False):
    """
    From per-kind counts, derive which gate each strategy passed/died at.

    Returns list of gate symbols and a fate string.

    Gate model:
      generate  — excluded (pgs_mask) vs passed
      precheck  — skipped (add_path_precheck) vs passed
      compete   — rejected (add_path) vs accepted
      frontier  — survived vs displaced/accepted-only
      policy    — (proposed only) forbidden vs won
    """
    excl  = kinds.get("excluded", 0)
    skip  = kinds.get("skipped", 0)
    rej   = kinds.get("rejected", 0)
    acc   = kinds.get("accepted", 0)
    disp  = kinds.get("displaced", 0)
    surv  = kinds.get("survived", 0)

    gates = GATES_PROPOSED if is_proposed else GATES_ORDINARY
    result = []

    def fill_skip():
        for _ in range(len(gates) - len(result)):
            result.append(G_SKIP)

    # generate gate: ✗ only if ALL excluded with nothing else
    if excl > 0 and acc == 0 and skip == 0 and rej == 0 and surv == 0:
        result.append(G_DEAD); fill_skip()
        return result, "EXCLUDED"
    result.append(G_PASS)

    # precheck gate: ✗ only if ALL skipped with nothing past
    if skip > 0 and acc == 0 and rej == 0 and surv == 0:
        result.append(G_DEAD); fill_skip()
        return result, "SKIPPED"
    # partial skip: some died here, others continued → show ✗ but continue
    result.append(G_DEAD if (skip > 0 and acc == 0 and rej == 0 and surv == 0)
                  else G_PASS if skip == 0 else G_DEAD)

    # compete gate: ✗ only if ALL rejected with nothing past
    if rej > 0 and acc == 0 and surv == 0:
        result.append(G_DEAD); fill_skip()
        return result, "REJECTED"
    result.append(G_DEAD if (rej > 0 and acc == 0 and surv == 0)
                  else G_PASS if rej == 0 else G_DEAD)

    # frontier gate
    if surv > 0:
        result.append(G_PASS)
        if is_proposed:
            result.append(G_PASS)
        return result, "survived ◄"
    elif acc > 0:
        result.append(G_DEAD)
        if is_proposed:
            result.append(G_FORB if winner_forbidden else G_DEAD)
            return result, ("accepted → forbidden ⊘" if winner_forbidden
                           else "accepted → not survived")
        return result, "accepted → not survived"
    else:
        result.append(G_SKIP); fill_skip()
        return result, "unknown"




# ---------------------------------------------------------------------------
# Matrix view — canonical gate view for both single candidate and aggregate
# ---------------------------------------------------------------------------
# Gate columns: gen pre cmp fr pol
# Single candidate (proposed wp): symbols ○ ✗ ⊘ ★
# Aggregate (strategy): count with dominant symbol
# ---------------------------------------------------------------------------

COL_PASS = "  ○  "
COL_DEAD = "  ✗  "
COL_SKIP = "  ─  "
COL_FORB = "  ⊘  "
COL_WIN  = "  ★  "   # raw-cost winner (but policy may block)

GATE_COLS  = ["gen", "pre", "cmp", "fr ", "pol"]
GATE_COLS_PROPOSED = ["gen", "pre", "cmp", "fr ", "pol"]
GATE_COLS_ORDINARY = ["gen", "pre", "cmp", "fr "]

COL_W = 5   # width per gate column
NAME_W = 28


def gate_sym_single(kinds, gate_idx, is_proposed, winner_forbidden):
    """Symbol for one gate, single-candidate mode."""
    excl = kinds.get("excluded", 0)
    skip = kinds.get("skipped",  0)
    rej  = kinds.get("rejected", 0)
    acc  = kinds.get("accepted", 0)
    surv = kinds.get("survived", 0)

    # gate_idx: 0=gen 1=pre 2=cmp 3=fr 4=pol
    if gate_idx == 0:  # generate
        if excl > 0 and acc == 0 and skip == 0 and rej == 0 and surv == 0:
            return COL_DEAD
        return COL_PASS

    if gate_idx == 1:  # precheck
        if skip > 0 and acc == 0 and rej == 0 and surv == 0:
            return COL_DEAD
        if skip > 0:
            return COL_DEAD
        return COL_PASS

    if gate_idx == 2:  # compete
        if rej > 0 and acc == 0 and surv == 0:
            return COL_DEAD
        if rej > 0:
            return COL_DEAD
        return COL_PASS

    if gate_idx == 3:  # frontier
        if surv > 0:
            return COL_WIN if (is_proposed and winner_forbidden) else COL_PASS
        if acc > 0:
            return COL_DEAD
        return COL_SKIP

    if gate_idx == 4:  # policy (proposed only)
        if not is_proposed:
            return "  ─  "
        if surv > 0:
            return COL_PASS
        if acc > 0 and winner_forbidden:
            return COL_FORB
        return COL_SKIP

    return COL_SKIP


def gate_sym_aggregate(kinds, gate_idx):
    """Symbol for one gate, aggregate (strategy) mode — count-aware."""
    excl = kinds.get("excluded", 0)
    skip = kinds.get("skipped",  0)
    rej  = kinds.get("rejected", 0)
    acc  = kinds.get("accepted", 0)
    surv = kinds.get("survived", 0)

    if gate_idx == 0:  # generate
        if excl > 0 and acc + skip + rej + surv == 0:
            return f"{excl}✗".center(COL_W)
        total = excl + acc + skip + rej + surv
        return f"{total}○".center(COL_W) if total else COL_SKIP

    if gate_idx == 1:  # precheck
        if skip > 0 and acc + rej + surv == 0:
            return f"{skip}✗".center(COL_W)
        if skip > 0:
            return f"{skip}✗".center(COL_W)
        passed = acc + rej + surv
        return f"{passed}○".center(COL_W) if passed else COL_SKIP

    if gate_idx == 2:  # compete
        if rej > 0 and acc + surv == 0:
            return f"{rej}✗".center(COL_W)
        if rej > 0:
            return f"{rej}✗".center(COL_W)
        passed = acc + surv
        return f"{passed}○".center(COL_W) if passed else COL_SKIP

    if gate_idx == 3:  # frontier
        if surv > 0:
            return f"{surv}○".center(COL_W)
        if acc > 0:
            return f"{acc}✗".center(COL_W)
        return COL_SKIP

    return COL_SKIP   # policy not used for aggregate


def result_label_single(kinds, is_proposed, winner_forbidden):
    surv = kinds.get("survived", 0)
    acc  = kinds.get("accepted", 0)
    rej  = kinds.get("rejected", 0)
    excl = kinds.get("excluded", 0)
    skip = kinds.get("skipped",  0)
    if excl > 0 and acc + skip + rej + surv == 0:
        return "excluded"
    if skip > 0 and acc + rej + surv == 0:
        return "skipped"
    if rej > 0 and acc + surv == 0:
        return "rejected"
    if surv > 0:
        if is_proposed and winner_forbidden:
            return "raw-cost winner / policy blocked"
        return "survived ◄"
    if acc > 0:
        if is_proposed and winner_forbidden:
            return "forbidden ⊘"
        return "accepted"
    return "unknown"


def result_label_aggregate(kinds):
    surv = kinds.get("survived", 0)
    acc  = kinds.get("accepted", 0)
    rej  = kinds.get("rejected", 0)
    skip = kinds.get("skipped",  0)
    excl = kinds.get("excluded", 0)
    if surv > 0:
        return f"survived ({surv})"
    if skip > 0 and rej == 0 and acc == 0:
        return "pre-dominant"
    if rej > 0 and acc == 0:
        return "cmp-dominant"
    if acc > 0 and surv == 0:
        return "no survivors"
    if excl > 0:
        return "excluded"
    return "unknown"


def diagnose_ordinary(kinds):
    """One-line diagnosis for aggregate strategy row."""
    excl = kinds.get("excluded", 0)
    skip = kinds.get("skipped",  0)
    rej  = kinds.get("rejected", 0)
    acc  = kinds.get("accepted", 0)
    surv = kinds.get("survived", 0)
    total = excl + skip + rej + acc + surv
    if total == 0:
        return "no events"
    if surv > 0:
        return "survived"   # ◄ marks this class
    # dominant death gate
    dominant = max(
        ("gen",  excl),
        ("pre",  skip),
        ("cmp",  rej),
        ("fr",   acc),   # accepted but no surv
        key=lambda x: x[1]
    )
    gate, count = dominant
    pct = int(100 * count / total)
    labels = {"gen": "excluded at generate",
              "pre": "mostly died at precheck",
              "cmp": "mostly died at compete",
              "fr":  "accepted but no survivors"}
    return labels.get(gate, "unknown")


def gate_label_ordinary(kinds, gate_idx):
    """Labeled gate cell: gen=40  pre=31✗  etc."""
    excl = kinds.get("excluded", 0)
    skip = kinds.get("skipped",  0)
    rej  = kinds.get("rejected", 0)
    acc  = kinds.get("accepted", 0)
    surv = kinds.get("survived", 0)

    if gate_idx == 0:
        total = excl + skip + rej + acc + surv
        sym = "✗" if (excl > 0 and total == excl) else "○"
        return f"gen={total}{sym}"
    if gate_idx == 1:
        passed = skip + rej + acc + surv
        if skip > 0 and passed == skip:
            return f"pre={skip}✗"
        return f"pre={skip}✗" if skip > 0 else f"pre={passed}○"
    if gate_idx == 2:
        passed = rej + acc + surv
        if rej > 0 and passed == rej:
            return f"cmp={rej}✗"
        return f"cmp={rej}✗" if rej > 0 else f"cmp={passed}○"
    if gate_idx == 3:
        if surv > 0:
            return f"fr={surv}○"
        if acc > 0:
            return f"fr={acc} lost"   # entered frontier but did not survive
        return "fr=─"
    return ""


def print_path_matrix(data, rel_filter=None, pathname=None):
    """
    Diagnosis-first gate view.

    Ordinary strategies: compact labeled profile + diagnosis
    Proposed candidate:  result card + labeled gate row
    """
    is_proposed = "candidate" in data
    rel_stats   = build_rel_stats(data)
    rels_map    = data.get("rels", {})
    wf          = data.get("candidate", {}).get("winner_forbidden", False) if is_proposed else False
    cand_name   = pathname or (data.get("candidate", {}).get("pathname") if is_proposed else None)
    fate_block  = data.get("fate", {}) if is_proposed else {}

    # Select rels
    if rel_filter:
        targets = {rel_filter: rel_stats[rel_filter]} if rel_filter in rel_stats else {}
    elif is_proposed:
        tgt_relname = data.get("target", {}).get("relname", "")
        tgt_id  = next((k for k, v in rels_map.items() if v == tgt_relname), "?")
        tgt_key = f"{{{tgt_id}}}"
        targets = {tgt_key: rel_stats[tgt_key]} if tgt_key in rel_stats else rel_stats
    else:
        sorted_rels = sorted(
            ((r, st) for r, st in rel_stats.items() if r != "?"),
            key=lambda x: x[1]["total"], reverse=True
        )
        targets = dict(sorted_rels[:3])

    for rel_key, stats in targets.items():
        rel_label = rel_key
        if is_proposed:
            rname = data.get("target", {}).get("relname", "")
            rel_label = f"{rel_key} ({rname})"

        query = data.get("query", "")
        print(f"GATE VIEW  {rel_label}")
        if query:
            # Truncate long queries for display
            q_display = query if len(query) <= 72 else query[:69] + "..."
            print(f"  query: {q_display}")
        print()

        strategies = sorted(
            stats["strategies"].items(),
            key=lambda x: (0 if x[0]=="CustomScan" and is_proposed
                          else sum(x[1].values())),
            reverse=True
        )

        # ── ORDINARY STRATEGIES ─────────────────────────────────────────
        ordinary = [(st, k) for st, k in strategies
                    if not (st == "CustomScan" and is_proposed)]
        if ordinary:
            print("  ORDINARY STRATEGIES")
            name_w = max(len(st) for st, _ in ordinary)
            name_w = max(name_w, 10)
            for strategy, kinds in ordinary:
                cells = "  ".join(
                    gate_label_ordinary(kinds, gi) for gi in range(4)
                )
                diag = diagnose_ordinary(kinds)
                surv = kinds.get("survived", 0)
                winner_mark = "  ◄" if surv > 0 else ""
                print(f"  {strategy:<{name_w}}  {cells}   → {diag}{winner_mark}")
            print()

        # ── PROPOSED CANDIDATE ──────────────────────────────────────────
        wp_kinds = next(
            (k for st, k in strategies if st == "CustomScan" and is_proposed),
            None
        )
        if is_proposed and cand_name:
            # Synthesize from fate block if wp_kinds missing
            if wp_kinds is None:
                fate_str = fate_block.get("fate", "unknown")
                wp_kinds = {"excluded":0,"skipped":0,"rejected":0,
                            "accepted":0,"survived":0}
                if "survived"  in fate_str: wp_kinds["survived"] = 1
                elif "accepted" in fate_str: wp_kinds["accepted"] = 1
                elif "rejected" in fate_str: wp_kinds["rejected"] = 1

            raw_best = data.get("candidate", {}).get("cost_total")
            eff_win_cost   = fate_block.get("winner_cost")
            eff_win_strat  = fate_block.get("winner_strategy", "")
            fate_str = fate_block.get("fate", "unknown")
            note     = fate_block.get("note", "")

            # Lifecycle label
            if fate_str == "survived":
                lifecycle = "survived ◄"
            elif "accepted" in fate_str and wf:
                lifecycle = "accepted → forbidden ⊘"
            elif "accepted" in fate_str:
                lifecycle = "accepted → not survived"
            elif "rejected" in fate_str:
                lifecycle = "rejected"
            else:
                lifecycle = fate_str

            # Reason
            if "policy" in note or "winner_forbidden" in note:
                reason = "winner_forbidden policy"
            elif "disabled_nodes" in note:
                reason = "disabled_nodes (winner_forbidden)"
            elif "genuine" in note:
                reason = "cost (genuinely lost)"
            else:
                reason = note or "─"

            print("  PROPOSED CANDIDATE")
            print(f"  {cand_name}")
            if raw_best is not None:
                rb_str = f"yes  ({raw_best:.2f})"
                ew_str = (f"no   ({eff_win_strat} @ {eff_win_cost:.2f})"
                          if eff_win_cost else "─")
                raw_won = raw_best < eff_win_cost if eff_win_cost else False
                print(f"    raw best      : {rb_str}")
                print(f"    effective win : {ew_str}")
            print(f"    lifecycle     : {lifecycle}")
            print(f"    reason        : {reason}")
            print()

            # Compact gate row
            gate_cells = []
            surv = wp_kinds.get("survived", 0)
            acc  = wp_kinds.get("accepted", 0)
            rej  = wp_kinds.get("rejected", 0)
            skip = wp_kinds.get("skipped",  0)
            excl = wp_kinds.get("excluded", 0)
            for gi, gname in enumerate(["gen", "pre", "cmp", "fr", "pol"]):
                sym = gate_sym_single(wp_kinds, gi, True, wf)
                sym = sym.strip()
                gate_cells.append(f"{gname}={sym}")
            print(f"    gates         : {'  '.join(gate_cells)}")
            print()

        # ── LEGEND ──────────────────────────────────────────────────────
        print(f"  ○ passed  ✗ first failing gate  ─ not reached  "
              f"⊘ policy blocked  ◄ effective winner  lost = entered frontier, did not survive")
        print()


def render_path_line(label, gate_syms, gates, fate_str, name_w=24):
    """
    Render a single path as a horizontal lifecycle line:

      label:  generate ──○── precheck ──○── compete ──✗── frontier ──▶ fate
    """
    PASS_SEG  = "──○──"
    DEAD_SEG  = "──✗──"
    SKIP_SEG  = "─────"
    FORB_SEG  = "──⊘──"
    ARROW     = "──▶"

    line = f"  {label:<{name_w}}  "
    dead = False
    for i, (sym, gate) in enumerate(zip(gate_syms, gates)):
        if i > 0:
            line += "── "
        line += gate + " "
        if dead:
            line += SKIP_SEG        # after death: grey dashes only
        elif sym == G_PASS:
            line += PASS_SEG
        elif sym == G_DEAD:
            line += DEAD_SEG
            dead = True
        elif sym == G_FORB:
            line += FORB_SEG
            dead = True
        else:
            line += SKIP_SEG
            dead = True
    line += ARROW + "  " + fate_str
    return line


def print_gate_lines(data, rel_filter=None, pathname=None):
    """
    Path lifecycle view: one horizontal line per strategy/candidate.
    Shows the journey through gates as a connected line.
    """
    is_proposed = "candidate" in data
    rel_stats   = build_rel_stats(data)
    rels_map    = data.get("rels", {})

    # Determine target rels
    if rel_filter:
        targets = {rel_filter: rel_stats[rel_filter]} if rel_filter in rel_stats else {}
    elif is_proposed:
        tgt_relname = data.get("target", {}).get("relname", "")
        tgt_id  = next((k for k, v in rels_map.items() if v == tgt_relname), "?")
        tgt_key = f"{{{tgt_id}}}"
        targets = {tgt_key: rel_stats[tgt_key]} if tgt_key in rel_stats else rel_stats
    else:
        sorted_rels = sorted(
            ((r, s) for r, s in rel_stats.items() if r != "?"),
            key=lambda x: x[1]["total"], reverse=True
        )
        targets = dict(sorted_rels[:3])

    gates = GATES_PROPOSED if is_proposed else GATES_ORDINARY
    wf    = data.get("candidate", {}).get("winner_forbidden", False) if is_proposed else False
    cand_name = pathname or (data.get("candidate", {}).get("pathname") if is_proposed else None)

    for rel_key, stats in targets.items():
        rel_name = ""
        if is_proposed:
            rel_name = f" ({data.get('target',{}).get('relname','')})"

        print(f"PATH LIFECYCLE  {rel_key}{rel_name}")
        print()

        strategies = sorted(
            stats["strategies"].items(),
            key=lambda x: sum(x[1].values()), reverse=True
        )

        # Inject wp row from fate block if needed
        rows = []
        has_wp = any(st == "CustomScan" for st, _ in strategies)
        if is_proposed and cand_name and not has_wp:
            fate_block = data.get("fate", {})
            fate_str   = fate_block.get("fate", "unknown")
            synth = {"excluded":0,"skipped":0,"rejected":0,"accepted":0,"survived":0}
            if fate_str == "survived":                synth["survived"] = 1
            elif fate_str == "accepted_not_survived": synth["accepted"] = 1
            elif fate_str == "rejected":              synth["rejected"] = 1
            gate_syms, gs = derive_gate_state(synth, is_proposed=True, winner_forbidden=wf)
            rows.append((cand_name, gate_syms, gs))

        for strategy, kinds in strategies:
            is_wp = (strategy == "CustomScan" and is_proposed)
            label = cand_name if is_wp and cand_name else strategy
            gate_syms, fate_str = derive_gate_state(
                kinds, is_proposed=is_wp, winner_forbidden=wf)
            rows.append((label, gate_syms, fate_str))

        name_w = max(len(r[0]) for r in rows)
        name_w = max(name_w, 14)

        for label, gate_syms, fate_str in rows:
            print(render_path_line(label, gate_syms, gates, fate_str, name_w))

        print()
        print(f"  Gates : {' → '.join(gates)}")
        print(f"  ○ passed   ✗ first failing gate   ─ not reached   ⊘ policy blocked   ★ raw-cost winner   ◄ effective winner")
        if not is_proposed:
            print("  Note  : each line = strategy-level aggregate")
        print()

def print_gate_diagram(data, rel_filter=None, pathname=None):
    """
    Gate diagram: lifecycle stages per strategy.

    ordinary:  generate → precheck → compete → frontier
    proposed:  generate → precheck → compete → frontier → policy
    """
    is_proposed = "candidate" in data
    rel_stats   = build_rel_stats(data)
    rels_map    = data.get("rels", {})

    # Determine which rels to show
    if rel_filter:
        targets = {rel_filter: rel_stats[rel_filter]} if rel_filter in rel_stats else {}
    elif is_proposed:
        tgt_relname = data.get("target", {}).get("relname", "")
        tgt_id = next((k for k, v in rels_map.items() if v == tgt_relname), "?")
        tgt_key = f"{{{tgt_id}}}"
        targets = {tgt_key: rel_stats[tgt_key]} if tgt_key in rel_stats else rel_stats
    else:
        # top 3 most active joinrels
        sorted_rels = sorted(
            ((r, s) for r, s in rel_stats.items() if r != "?"),
            key=lambda x: x[1]["total"], reverse=True
        )
        targets = dict(sorted_rels[:3])

    gates = GATES_PROPOSED if is_proposed else GATES_ORDINARY
    gate_w = 9
    name_w = 24

    for rel_key, stats in targets.items():
        # Resolve rel label
        rel_name = ""
        if "{" not in rel_key:
            rel_name = f" ({rels_map.get(rel_key, '')})"
        elif is_proposed:
            tgt = data.get("target", {}).get("relname", "")
            rel_name = f" ({tgt})"

        print(f"GATE DIAGRAM  {rel_key}{rel_name}")
        print("─" * (name_w + gate_w * len(gates) + 20))

        # Header
        header = f"  {'strategy':<{name_w}}"
        for g in gates:
            header += f"{g:^{gate_w}}"
        header += "  fate"
        print(header)
        print("  " + "─" * (name_w + gate_w * len(gates) + 10))

        # One row per strategy
        strategies = sorted(
            stats["strategies"].items(),
            key=lambda x: sum(x[1].values()), reverse=True
        )

        wf = data.get("candidate", {}).get("winner_forbidden", False) if is_proposed else False
        cand_name = pathname or (data.get("candidate", {}).get("pathname") if is_proposed else None)

        # For proposed: inject wp row from fate block if CustomScan not in summary
        if is_proposed and cand_name:
            has_wp = any(st == "CustomScan" for st, _ in strategies)
            if not has_wp:
                fate_block = data.get("fate", {})
                fate_str   = fate_block.get("fate", "unknown")
                # Synthesize kinds from fate
                synth = {"excluded":0,"skipped":0,"rejected":0,"accepted":0,"survived":0}
                if fate_str == "survived":               synth["survived"] = 1
                elif fate_str == "accepted_not_survived":synth["accepted"] = 1
                elif fate_str == "rejected":             synth["rejected"] = 1
                gate_syms, gs = derive_gate_state(synth, is_proposed=True, winner_forbidden=wf)
                row = f"  {cand_name:<{name_w}}"
                for sym in gate_syms:
                    row += f"{sym:^{gate_w}}"
                row += f"  {gs}"
                print(row)

        for strategy, kinds in strategies:
            is_wp = (strategy == "CustomScan" and is_proposed)
            label = cand_name if is_wp and cand_name else strategy

            gate_syms, fate_str = derive_gate_state(
                kinds, is_proposed=is_wp, winner_forbidden=wf
            )

            row = f"  {label:<{name_w}}"
            for sym in gate_syms:
                row += f"{sym:^{gate_w}}"
            row += f"  {fate_str}"
            print(row)

        print()

        # Legend
        print(f"  {G_PASS} passed          "
              f"{G_DEAD} first failing gate  "
              f"{G_SKIP} not reached")
        print(f"  {G_FORB} policy blocked  "
              f"{G_WIN}  raw-cost winner     "
              f"◄  effective winner")
        print()
        print(f"  Gates : {' → '.join(gates)}")
        if is_proposed:
            print("  Note  : proposed row = candidate lifecycle (one object)")
        else:
            print("  Note  : each row = strategy-level aggregate, "
                  "not a single path instance")
        print()



def print_candidates_table(data):
    """Multi-candidate view: candidates[] is the primary machine model."""
    cands = data.get("candidates") or []
    if not cands:
        return False
    print()
    print("CANDIDATES")
    print(hr())
    for c in cands:
        ec = c.get("event_counts", {})
        line = (f"{c.get('pathname','?'):<14} target={c.get('target','?'):<10} "
                f"fate={c.get('fate','?'):<22} cost={c.get('cost_total','?')}")
        if c.get("gap") is not None:
            line += f" gap={c['gap']:+.2f}"
        line += (f"  acc={ec.get('accepted',0)} rej={ec.get('rejected',0)}"
                 f" surv={ec.get('survived',0)}")
        if not c.get("injected", True):
            line += "  [not injected]"
        print(line)
    return True


def print_advisor_view(data):
    """Advisor + hypo_indexes machine blocks."""
    adv = data.get("advisor") or {}
    hyps = data.get("hypo_indexes") or []
    if not adv.get("enabled") and not hyps:
        print("no advisor data in input")
        return
    print()
    print("ADVISOR")
    print(hr())
    if adv.get("atoms"):
        print(f"atoms   : {adv['atoms']}")
    if "baseline_cost" in adv and "plan_cost" in adv:
        print(f"plan    : {adv['baseline_cost']:.2f} -> {adv['plan_cost']:.2f}"
              f"  (benefit {adv.get('query_level_benefit', 0):+.2f})")
    if adv.get("truncated"):
        print("WARNING : advisor candidate list truncated (cap hit)")
    for h in hyps:
        mark = "IN PLAN" if h.get("in_plan") else ("injected" if h.get("injected") else "not injected")
        best = f"  best={h['best_cost']:.2f}" if "best_cost" in h else ""
        print(f"  {h.get('name','?'):<10} [{h.get('ddl','?')}]  "
              f"acc={h.get('accepted',0)} rej={h.get('rejected',0)} "
              f"surv={h.get('survived',0)}{best}  {mark}")


def agent_gate_decision(data):
    """Deterministic policy layer: GREEN / YELLOW / RED + reasons.
    Not an LLM decision - simple explicit rules over machine fields."""
    reasons = []
    limits = data.get("limits") or {}
    adv = data.get("advisor") or {}
    plan_cost = adv.get("plan_cost")
    benefit = adv.get("query_level_benefit", 0.0) or 0.0
    baseline = adv.get("baseline_cost")
    in_plan_hypo = [h for h in (data.get("hypo_indexes") or [])
                    if h.get("in_plan")]
    truncated = (limits.get("events_truncated")
                 or limits.get("summary_truncated")
                 or adv.get("truncated"))
    j2 = sum(1 for e in data.get("events", []) if e.get("src") == "j2")

    cost = plan_cost if plan_cost is not None else baseline
    risk = "green"
    if truncated:
        risk = "red"
        reasons.append("observability_truncated")
    if cost is not None and cost > 100000:
        risk = "red"
        reasons.append("huge_plan_cost")
    if in_plan_hypo and benefit > 0 and baseline and benefit > 0.5 * baseline:
        # a missing index dominates the query: executing as-is wastes the server
        if risk != "red":
            risk = "red"
        reasons.append("advisor_found_dominant_in_plan_index")
    if risk == "green":
        if (cost is not None and cost > 1000) or j2 > 50:
            risk = "yellow"
            if cost is not None and cost > 1000:
                reasons.append("medium_plan_cost")
            if j2 > 50:
                reasons.append("high_join_pressure")
        elif in_plan_hypo and benefit > 0:
            risk = "yellow"
            reasons.append("advisor_found_useful_index")
    action = {"green": "execute",
              "yellow": "run_on_replica_with_timeout",
              "red": "refuse_or_suggest_index"}[risk]
    return risk, reasons or ["no_findings"], action


def print_agent_gate(data):
    risk, reasons, action = agent_gate_decision(data)
    print()
    print("AGENT GATE")
    print(hr())
    print(f"risk   : {risk}")
    print(f"reason : {', '.join(reasons)}")
    print(f"action : {action}")
    lim = data.get("limits") or {}
    if lim:
        print(f"limits : events_truncated={lim.get('events_truncated')}"
              f" summary_truncated={lim.get('summary_truncated')}")


def main():
    parser = argparse.ArgumentParser(
        description="pathprobe_report — summary-first report from pathprobe_json() output",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "QUICK START\n"
            "  Ordinary:\n"
            "    psql -t -A -c \"SELECT pathprobe_json(...)\" | python pathprobe_report.py -\n"
            "    psql -t -A -c \"SELECT pathprobe_json(...)\" \\\n"
            "      | python pathprobe_report.py - --gate --rel \"{1,2,4}\"   # (bash)\n"
            "  Proposed:\n"
            "    psql -t -A -c \"SELECT pathprobe_propose_json(...)\" \\\n"
            "      | python pathprobe_report.py - --gate               # (bash)\n"
            "\n"
            "GATE MATRIX (--gate)\n"
            "  python pathprobe_report.py out.json --gate --rel \"{1,2,4}\"\n"
            "  python pathprobe_report.py propose.json --gate\n"
            "  python pathprobe_report.py propose.json --gate --pathname duckdb_scan_filter\n"
            "\n"
            "SOURCE TAGS: p0=rel_frontier  p1=add_path  pc=precheck  j2=join  wp=proposed\n"
            "KIND VALUES: excluded skipped rejected accepted displaced survived\n"
            "\n"
            "GATE SYMBOLS: ○ passed  ✗ first failing gate  ─ not reached  ⊘ policy blocked\n"
            "           lost = entered frontier, did not survive  ◄ effective winner\n"
            "GATE COLUMNS: gen=generate  pre=precheck  cmp=compete  fr=frontier  pol=policy\n"
            "\n"
            "TYPICAL WORKFLOW:\n"
            "  1. default view         out.json\n"
            "  2. gate matrix          --gate --rel \"{1,2,4}\"\n"
            "  3. drill down           --rel \"{1,2,4}\" --strategy NestLoop --kind skipped\n"
            "  4. proposed experiment  propose.json  or  propose.json --gate\n"
            "  5. winners only         --winner-only --no-baserel\n"
        )
    )
    parser.add_argument("input", help="JSON file from pathprobe_json() or pathprobe_propose_json(), or '-' for stdin")
    parser.add_argument("--top", type=int, default=5,
                        help="number of top hotspots to show (default: 5)")
    parser.add_argument("--sort", choices=["events", "pressure", "rejected", "skipped"],
                        default="events",
                        help="hotspot sort key: events|pressure|rejected|skipped (default: events)")
    parser.add_argument("--rel", default=None,
                        help='filter by rel, e.g. --rel "{1,2,3}"')
    parser.add_argument("--strategy", default=None,
                        help="filter by pathtype/strategy, e.g. --strategy HashJoin")
    parser.add_argument("--kind", default=None,
                        help="filter by event kind: rejected|accepted|skipped|survived|excluded|displaced")
    parser.add_argument("--src", default=None,
                        help="filter by source: p0|p1|pc|j2|wp")
    parser.add_argument("--limit", type=int, default=50,
                        help="max raw trace events to show (default: 50)")
    parser.add_argument("--winner-only", action="store_true",
                        help="show only surviving paths (winners) across all rels")
    parser.add_argument("--no-baserel", action="store_true",
                        help="with --winner-only: hide baserel entries, show join tree only")
    parser.add_argument("--raw-all", action="store_true",
                        help="show full raw trace (respects other filters + --limit)")
    parser.add_argument("--profile", default=None,
                        help='show strategy profile for specific rel, e.g. --profile "{1,2}"')
    parser.add_argument("--advisor", action="store_true",
                        help="advisor + hypo_indexes machine view")
    parser.add_argument("--agent", action="store_true",
                        help="deterministic agent gate decision (green/yellow/red)")
    parser.add_argument("--gate", action="store_true",
                        help="gate diagram: show lifecycle stages for each strategy")
    parser.add_argument("--pathname", default=None,
                        help="with --gate: filter to proposed path by pathname")

    args = parser.parse_args()

    data = load_json(args.input)
    rel_stats = build_rel_stats(data)

    if args.agent:
        print_agent_gate(data)
        return
    if args.advisor:
        print_advisor_view(data)
        return

    # Detect pathprobe_propose_json() output — has "candidate" field
    if "candidate" in data and not any([args.rel, args.strategy, args.kind,
                                        args.src, args.raw_all, args.profile,
                                        args.winner_only, args.gate]):
        if not print_candidates_table(data):
            print_candidate_card(data)
        print_path_matrix(data, pathname=data.get("candidate", {}).get("pathname"))
        return

    have_filters = any([args.rel, args.strategy, args.kind, args.src, args.raw_all, args.profile, args.winner_only, args.gate])

    if not have_filters:
        # Default: global summary + top hotspot profiles
        print_global_summary(data, rel_stats, args.top, args.sort)
        print_top_hotspot_profiles(data, rel_stats, args.top)
    else:
        if args.gate:
            print_path_matrix(data, rel_filter=args.rel, pathname=args.pathname)
            return

        if args.profile:
            # Just the profile for requested rel
            print_hotspot_profile(data, rel_stats, args.profile)

        if args.winner_only:
            print_winners(data, rel_stats, no_baserel=args.no_baserel)
        elif args.rel or args.strategy or args.kind or args.src or args.raw_all:
            print_raw_trace(
                data,
                rel_filter=args.rel,
                strategy_filter=args.strategy,
                kind_filter=args.kind,
                src_filter=args.src,
                limit=args.limit,
            )


if __name__ == "__main__":
    main()
