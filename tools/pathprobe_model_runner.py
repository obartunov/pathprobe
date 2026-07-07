#!/usr/bin/env python3
"""
pathprobe model-based test runner + oracle.

Executes the explicit model cases from pathprobe_model_cases.py against a live
PostgreSQL cluster and checks the declared expectations.  The model is the
source of truth; SQL is emitted only on request (--emit-sql) for debugging.

Connection comes from the standard PG* environment (PGHOST/PGPORT/PGUSER/
PGDATABASE) plus PP_PSQL for the psql binary.  The runner installs its own
fixtures (schema ppm, ppm_hidden, roles ppm_*) and drops them on teardown.

Modes:
  smoke     mandatory model cases (default)
  pairwise  all-pairs over decided security combinations + parallelism matrix
  targeted  explicit 3-wise coverage of known risky interaction groups
  fuzz      randomized decided combinations (--seed)

Other actions:
  --list-cases   list case ids + dimensions + expected oracle
  --emit-matrix  print the coverage matrix (dimension value -> case count)
  --emit-sql     print the per-case SQL scripts (debug/export only)
  --format tap   emit TAP instead of the text summary
"""

import argparse
import base64
import json
import os
import random
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pathprobe_model_cases as M  # noqa: E402

US = "\x1f"   # unit separator: marker/payload
RS = "\x1e"   # record separator: fields inside ppm_call return

GUC_WATCH = [
    "enable_seqscan", "enable_indexscan", "enable_bitmapscan",
    "enable_indexonlyscan", "enable_sort", "enable_hashjoin",
    "enable_nestloop", "enable_mergejoin",
]

DENIED_SQLSTATES = {"42501", "0A000"}   # insufficient_privilege, feature_not_supported


# --------------------------------------------------------------------------
# psql plumbing
# --------------------------------------------------------------------------

def psql_bin():
    return os.environ.get("PP_PSQL", "psql")


def run_sql(script: str) -> str:
    """Run a script in one psql session; return raw stdout."""
    cmd = [psql_bin(), "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0",
           "-P", "null=", "-f", "-"]
    p = subprocess.run(cmd, input=script, capture_output=True, text=True)
    if p.returncode != 0 and "ON_ERROR_STOP" not in script:
        # ON_ERROR_STOP is off, so a nonzero exit is unusual (connection fail)
        sys.stderr.write(p.stderr)
    return p.stdout


def _dollar(tag: str, s: str) -> str:
    assert f"${tag}$" not in s, f"delimiter ${tag}$ collides with case text"
    return f"${tag}${s}${tag}$"


# --------------------------------------------------------------------------
# fixtures
# --------------------------------------------------------------------------

def setup():
    run_sql(M.SETUP_SQL)


def teardown():
    run_sql(M.TEARDOWN_SQL)


# --------------------------------------------------------------------------
# case execution
# --------------------------------------------------------------------------

def _call_row(fn, query, spec):
    # base64 the whole ppm_call return so newlines in JSON cannot break parsing
    return ("SELECT 'RES'||chr(31)||translate("
            "encode(convert_to(ppm_call(%s, %s, %s),'UTF8'),'base64'), E'\\n','')"
            % (_dollar("FN", fn), _dollar("Q", query), _dollar("S", spec)))


def _guc_row(when):
    names = ",".join("'%s'" % n for n in GUC_WATCH)
    return ("SELECT 'GUC'||chr(31)||'%s'||chr(31)||"
            "coalesce(string_agg(name||'='||setting, ',' ORDER BY name),'') "
            "FROM pg_settings WHERE name IN (%s);" % (when, names))


def build_security_script(c: "M.SecurityCase"):
    sqlrole = M.ROLE_SQL[c.role]
    parts = []
    if sqlrole is not None:
        parts.append("SET ROLE %s;" % sqlrole)
    parts.append("SET search_path = ppm, public;")
    want_guc = "NO_GUC_LEAK" in c.expect
    if want_guc:
        parts.append(_guc_row("BEFORE"))
    parts.append(_call_row(c.fn, c.query, c.spec) + ";")
    if want_guc:
        parts.append(_guc_row("AFTER"))
    if sqlrole is not None:
        parts.append("RESET ROLE;")
    return "\n".join(parts) + "\n"


def build_correctness_script(c: "M.CorrectnessCase"):
    parts = ["RESET ROLE;", "SET search_path = ppm, public;"]
    if c.parallelism == "pinned_off":
        parts.append("SET max_parallel_workers_per_gather = 0;")
    elif c.parallelism == "forced_on":
        parts += ["SET max_parallel_workers_per_gather = 4;",
                  "SET min_parallel_table_scan_size = 0;",
                  "SET parallel_setup_cost = 0;",
                  "SET parallel_tuple_cost = 0;"]
    if c.max_events is not None:
        parts.append("SET pathprobe.max_events = %d;" % c.max_events)
    parts.append(_call_row(c.fn, c.query, c.spec) + ";")
    parts.append("RESET ALL;")
    return "\n".join(parts) + "\n"


def parse_output(raw: str):
    """Return (status, sqlstate, message, payload, gucs) from marker lines."""
    status = sqlstate = message = payload = None
    gucs = {}
    for line in raw.splitlines():
        if line.startswith("RES" + US):
            b64 = line[len("RES" + US):]
            try:
                dec = base64.b64decode(b64).decode("utf-8", "replace")
            except Exception:
                dec = ""
            fields = dec.split(RS)
            status = fields[0] if fields else None
            if status == "OK":
                sqlstate = ""
                payload = fields[2] if len(fields) > 2 else ""
            else:  # ERR
                sqlstate = fields[1] if len(fields) > 1 else ""
                message = fields[2] if len(fields) > 2 else ""
        elif line.startswith("GUC" + US):
            rest = line[len("GUC" + US):]
            when, _, vals = rest.partition(US)
            gucs[when] = vals
    return status, sqlstate, message, payload, gucs


# --------------------------------------------------------------------------
# security oracle
# --------------------------------------------------------------------------

def _token_present(name, text):
    """True if `name` appears in `text` as a standalone identifier token, i.e.
    not flanked by identifier characters.  This prevents 'private' from matching
    inside 'v_private' and lets a qualified 'ppm.private' match as its own token
    while a bare 'private' does not match inside it."""
    pat = r'(?<![\w.])' + re.escape(name) + r'(?![\w])'
    return re.search(pat, text) is not None


def check_security(c, status, sqlstate, message, payload, gucs):
    fails = []
    text = (message or "") + (payload or "")

    if "OK" in c.expect:
        if status != "OK":
            fails.append("expected OK, got %s/%s: %s"
                         % (status, sqlstate, (message or "")[:120]))
        elif not _looks_like_json(payload):
            fails.append("OK but payload is not valid JSON")

    if "DENIED" in c.expect:
        if status == "OK":
            fails.append("expected DENIED, got OK")
        elif sqlstate not in DENIED_SQLSTATES:
            fails.append("expected DENIED (permission error), got sqlstate %s: %s"
                         % (sqlstate, (message or "")[:120]))

    if "NO_LEAK" in c.expect:
        supplied = (c.query or "") + " " + (c.spec or "")
        for name in c.forbidden_names:
            if not name:
                continue
            # a name the caller itself referenced is not a hidden leak
            if _token_present(name, supplied):
                continue
            if _token_present(name, text):
                fails.append("NO_LEAK violated: forbidden name %r in output/error"
                             % name)

    if "NO_GUC_LEAK" in c.expect:
        before = gucs.get("BEFORE")
        after = gucs.get("AFTER")
        if before is None or after is None:
            fails.append("NO_GUC_LEAK: missing GUC snapshot")
        elif before != after:
            fails.append("NO_GUC_LEAK violated: %s != %s" % (before, after))

    return fails


def _looks_like_json(payload):
    if not payload:
        return False
    try:
        json.loads(payload)
        return True
    except Exception:
        return False


# --------------------------------------------------------------------------
# correctness oracle
# --------------------------------------------------------------------------

def check_correctness(c, status, sqlstate, message, payload):
    fails = []
    if status != "OK":
        fails.append("call failed (%s): %s" % (sqlstate, (message or "")[:160]))
        return fails
    try:
        j = json.loads(payload)
    except Exception as e:
        fails.append("payload not JSON: %s" % e)
        return fails

    for fld in c.require_fields:
        if fld not in j:
            fails.append("missing required field %r" % fld)

    # events[].seq strictly increasing
    events = j.get("events", [])
    seqs = [e.get("seq") for e in events if isinstance(e, dict) and "seq" in e]
    if any(seqs[i] >= seqs[i + 1] for i in range(len(seqs) - 1)):
        fails.append("events[].seq not strictly increasing")
    if c.expect_truncated is not None:
        lim = j.get("limits", {})
        trunc = bool(lim.get("events_truncated"))
        if trunc != c.expect_truncated:
            fails.append("events_truncated=%s, expected %s"
                         % (trunc, c.expect_truncated))
        cap = lim.get("max_events")
        if cap is not None and len(events) > cap:
            fails.append("emitted %d events exceeds cap %d" % (len(events), cap))

    # candidate invariants
    for cand in j.get("candidates", []):
        if not isinstance(cand, dict):
            continue
        ec = cand.get("event_counts", {})
        acc, sur = ec.get("accepted", 0), ec.get("survived", 0)
        if sur > acc:
            fails.append("candidate survived(%d) > accepted(%d)" % (sur, acc))
        if cand.get("fate") == "not_injected":
            if any(ec.get(k, 0) for k in ("accepted", "rejected", "survived")):
                fails.append("not_injected but nonzero event_counts")
        if cand.get("fate") not in M.FATE_ALPHABET:
            fails.append("fate %r outside alphabet" % cand.get("fate"))

    # allowed_fates (first candidate)
    if c.allowed_fates is not None:
        cands = j.get("candidates", [])
        if not cands:
            fails.append("expected a candidate with a fate, none present")
        else:
            fate = cands[0].get("fate")
            if fate not in c.allowed_fates:
                fails.append("fate %r not in allowed_fates %s"
                             % (fate, sorted(c.allowed_fates)))

    # hypo invariants
    for h in j.get("hypo_indexes", []):
        if not isinstance(h, dict):
            continue
        if h.get("in_plan") and not h.get("injected"):
            fails.append("hypo in_plan but not injected")
        if h.get("survived", 0) > h.get("accepted", 0):
            fails.append("hypo survived > accepted")

    # advisor invariants
    if c.advisor_expected:
        adv = j.get("advisor", {})
        if not adv.get("enabled"):
            fails.append("advisor expected enabled")
        else:
            if "baseline_cost" not in adv:
                fails.append("advisor.enabled but no baseline_cost")
            if "plan_cost" not in adv:
                fails.append("advisor.enabled but no plan_cost")
            if all(k in adv for k in ("baseline_cost", "plan_cost",
                                      "query_level_benefit")):
                b, p, q = (adv["baseline_cost"], adv["plan_cost"],
                           adv["query_level_benefit"])
                if abs((b - p) - q) > 1e-6:
                    fails.append("query_level_benefit != baseline-plan")
    return fails


# --------------------------------------------------------------------------
# drivers
# --------------------------------------------------------------------------

def run_security(cases):
    results = []
    for c in cases:
        raw = run_sql(build_security_script(c))
        status, st, msg, payload, gucs = parse_output(raw)
        fails = check_security(c, status, st, msg, payload, gucs)
        results.append((c.cid, not fails, fails))
    return results


def run_correctness(cases):
    results = []
    for c in cases:
        raw = run_sql(build_correctness_script(c))
        status, st, msg, payload, _ = parse_output(raw)
        fails = check_correctness(c, status, st, msg, payload)
        results.append((c.cid, not fails, fails))
    return results


def collect(mode, seed):
    base = "pairwise" if mode == "fuzz" else mode
    sec, skips = M.generate_security_ex(base)
    cor = M.generate_correctness(base)
    if mode == "fuzz":
        rnd = random.Random(seed)
        rnd.shuffle(sec)
        rnd.shuffle(cor)
        sec = sec[:max(1, len(sec) // 3)]
        cor = cor[:max(1, len(cor) // 2)]
    return sec, cor, skips


def skip_summary(skips):
    """Aggregate (dims, reason) skips into counts by reason."""
    from collections import Counter
    c = Counter(reason for _, reason in skips)
    return c


# --------------------------------------------------------------------------
# reporting / actions
# --------------------------------------------------------------------------

def action_list(sec, cor):
    print("# security cases (%d)" % len(sec))
    for c in sec:
        print("  %-52s role=%-20s expect=%s"
              % (c.cid, c.role, ",".join(sorted(c.expect))))
    print("# correctness cases (%d)" % len(cor))
    for c in cor:
        fates = ",".join(sorted(c.allowed_fates)) if c.allowed_fates else "-"
        print("  %-52s par=%-10s fates=%s" % (c.cid, c.parallelism, fates))


def action_skips(skips):
    print("# skipped combinations (%d) by reason" % len(skips))
    for reason, n in skip_summary(skips).most_common():
        print("  %3d  %s" % (n, reason))
    print("# examples")
    seen = set()
    for dims, reason in skips:
        if reason in seen:
            continue
        seen.add(reason)
        key = "/".join(str(dims.get(k)) for k in
                       ("role", "relation", "query_shape", "spec_shape"))
        print("  %-70s <- %s" % (key, reason))


def action_matrix(sec, cor, skips=None):
    print("# coverage matrix (security)")
    for dim, values in (("role", M.ROLES), ("relation", M.RELATIONS),
                        ("query_shape", M.QUERY_SHAPES),
                        ("spec_shape", M.SPEC_SHAPES)):
        counts = {v: 0 for v in values}
        for c in sec:
            v = c.role if dim == "role" else c.dims.get(dim)
            if v in counts:
                counts[v] += 1
        print("  %-12s %s" % (dim,
              ", ".join("%s=%d" % (v, counts[v]) for v in values)))
    print("# coverage matrix (correctness)")
    for dim, values in (("parallelism", M.PARALLELISM), ("policy", M.POLICIES)):
        counts = {v: 0 for v in values}
        for c in cor:
            v = c.dims.get(dim)
            if v in counts:
                counts[v] += 1
        print("  %-12s %s" % (dim,
              ", ".join("%s=%d" % (v, counts[v]) for v in values)))
    if skips:
        print("# skipped (%d) by reason" % len(skips))
        for reason, n in skip_summary(skips).most_common():
            print("  %3d  %s" % (n, reason))


def action_emit_sql(sec, cor):
    for c in sec:
        print("-- SECURITY %s (expect %s)" % (c.cid, ",".join(sorted(c.expect))))
        print(build_security_script(c))
    for c in cor:
        print("-- CORRECTNESS %s" % c.cid)
        print(build_correctness_script(c))


def report(kind, results, fmt):
    npass = sum(1 for _, ok, _ in results if ok)
    if fmt == "tap":
        print("# %s" % kind)
        print("1..%d" % len(results))
        for i, (cid, ok, fails) in enumerate(results, 1):
            print("%s %d - %s" % ("ok" if ok else "not ok", i, cid))
            for f in fails:
                print("  # %s" % f)
    else:
        for cid, ok, fails in results:
            if not ok:
                print("FAIL %s" % cid)
                for f in fails:
                    print("      %s" % f)
        print("%s: %d/%d passed" % (kind, npass, len(results)))
    return npass == len(results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="smoke",
                    choices=["smoke", "pairwise", "targeted", "fuzz"])
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--format", default="text", choices=["text", "tap"])
    ap.add_argument("--list-cases", action="store_true")
    ap.add_argument("--list-skips", action="store_true")
    ap.add_argument("--emit-matrix", action="store_true")
    ap.add_argument("--emit-sql", action="store_true")
    ap.add_argument("--no-setup", action="store_true",
                    help="assume fixtures already installed")
    ap.add_argument("--keep", action="store_true",
                    help="do not tear down fixtures")
    ap.add_argument("--only", choices=["security", "correctness"])
    args = ap.parse_args()

    sec, cor, skips = collect(args.mode, args.seed)
    if args.only == "security":
        cor = []
    elif args.only == "correctness":
        sec = []
        skips = []

    if args.list_cases:
        action_list(sec, cor)
        return 0
    if args.list_skips:
        action_skips(skips)
        return 0
    if args.emit_matrix:
        action_matrix(sec, cor, skips)
        return 0
    if args.emit_sql:
        action_emit_sql(sec, cor)
        return 0

    if not args.no_setup:
        setup()
    try:
        ok = True
        if sec:
            ok = report("security", run_security(sec), args.format) and ok
        if cor:
            ok = report("correctness", run_correctness(cor), args.format) and ok
    finally:
        if not args.keep and not args.no_setup:
            teardown()
    if skips and args.format != "tap":
        print("skipped: %d combinations (see --list-skips)" % len(skips))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
