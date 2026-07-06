#!/usr/bin/env python3
# gen_pathprobe_tests.py - part of pathprobe.
# Distributed under the PostgreSQL License; see the repository LICENSE file.
"""
NOTE: --expected-out emits a draft; the canonical expected file is the
live pg_regress run output (results/ -> expected/), per project rule.

Generate pathprobe combinatorial invariant tests.

The generated SQL intentionally checks semantic invariants instead of exact
planner costs.  It is suitable for regression tests across PostgreSQL versions
where costs and event counts may drift but the JSON contract must remain stable.

Usage:
  python tools/gen_pathprobe_tests.py --mode smoke --sql-out sql/pathprobe_combo_generated.sql --expected-out expected/pathprobe_combo_generated.out
  python tools/gen_pathprobe_tests.py --mode pairwise --sql-out /tmp/pathprobe_pairwise.sql
  python tools/gen_pathprobe_tests.py --mode full --sql-out /tmp/pathprobe_full.sql
  python tools/gen_pathprobe_tests.py --mode smoke --list-cases
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


DIMENSIONS: Dict[str, Sequence[object]] = {
    "rel": ["combo", "no_such_rel"],
    "forbid": [True, False],
    "cheap": [True, False],
    "ordered": [False, True],
    "hypoform": ["none", "json", "sql", "advise"],
    "cap": ["normal", "small"],
}

SMOKE_CASES = [
    dict(rel="combo", forbid=True, cheap=True, ordered=False, hypoform="none", cap="normal"),
    dict(rel="combo", forbid=True, cheap=False, ordered=False, hypoform="none", cap="normal"),
    dict(rel="no_such_rel", forbid=True, cheap=True, ordered=False, hypoform="none", cap="normal"),
    dict(rel="combo", forbid=True, cheap=True, ordered=True, hypoform="none", cap="normal"),
    dict(rel="combo", forbid=True, cheap=True, ordered=False, hypoform="json", cap="normal"),
    dict(rel="combo", forbid=True, cheap=True, ordered=False, hypoform="sql", cap="normal"),
    dict(rel="combo", forbid=True, cheap=True, ordered=False, hypoform="advise", cap="normal"),
    dict(rel="combo", forbid=True, cheap=True, ordered=False, hypoform="none", cap="small"),
]

QUERY = "SELECT * FROM combo WHERE v > 90 ORDER BY id"


@dataclass(frozen=True)
class Case:
    name: str
    rel: str
    forbid: bool
    cheap: bool
    ordered: bool
    hypoform: str
    cap: str

    @property
    def cap_small(self) -> bool:
        return self.cap == "small"

    def spec(self) -> dict:
        suffix = self.name_hash()
        spec = {
            "target": {"relname": self.rel},
            "candidate": {"pathname": f"c_{suffix}", "provider": "combo-gen"},
            "cost": {
                "total_cost": 0.5 if self.cheap else 99999.0,
                "rows": 10,
            },
            "mode": {"winner_forbidden": self.forbid},
        }
        if self.ordered:
            spec["planner"] = {"pathkeys": ["id"]}
        if self.hypoform == "json":
            spec["hypo_indexes"] = [
                {"relname": "combo", "name": f"hj_{suffix}", "columns": ["v"]}
            ]
        elif self.hypoform == "sql":
            spec["hypo_sql"] = [
                f"CREATE INDEX hs_{suffix} ON combo (v) INCLUDE (id)"
            ]
        elif self.hypoform == "advise":
            spec["advise"] = True
        elif self.hypoform != "none":
            raise ValueError(f"unknown hypoform: {self.hypoform}")
        return spec

    def name_hash(self) -> str:
        return hashlib.sha1(self.name.encode("utf-8")).hexdigest()[:8]


def case_name(values: dict) -> str:
    parts = [
        f"rel_{values['rel']}",
        f"forb_{str(values['forbid']).lower()}",
        "cheap" if values["cheap"] else "expensive",
        "ordered" if values["ordered"] else "unordered",
        f"hypo_{values['hypoform']}",
        f"cap_{values['cap']}",
    ]
    raw = "__".join(parts)
    return raw.replace("no_such_rel", "missing")


def make_case(values: dict) -> Case:
    kwargs = dict(values)
    # Use external dimension name "forbid" but internal field "forbid".
    return Case(name=case_name(kwargs), **kwargs)


def all_cases() -> List[Case]:
    keys = list(DIMENSIONS)
    out = []
    for combo in itertools.product(*(DIMENSIONS[k] for k in keys)):
        values = dict(zip(keys, combo))
        out.append(make_case(values))
    return out


def smoke_cases() -> List[Case]:
    return [make_case(v) for v in SMOKE_CASES]


def pairwise_cases() -> List[Case]:
    """Small greedy pairwise selector over the full product."""
    keys = list(DIMENSIONS)
    universe = set()
    for a, b in itertools.combinations(keys, 2):
        for av in DIMENSIONS[a]:
            for bv in DIMENSIONS[b]:
                universe.add(((a, av), (b, bv)))

    selected: List[Case] = []
    remaining = set(universe)
    candidates = all_cases()

    def pairs_for(c: Case):
        values = c.__dict__.copy()
        values["forbid"] = values.pop("forbid")
        # Remove derived/name fields.
        values = {
            "rel": c.rel,
            "forbid": c.forbid,
            "cheap": c.cheap,
            "ordered": c.ordered,
            "hypoform": c.hypoform,
            "cap": c.cap,
        }
        pairs = set()
        for a, b in itertools.combinations(keys, 2):
            pairs.add(((a, values[a]), (b, values[b])))
        return pairs

    while remaining:
        best = max(candidates, key=lambda c: len(pairs_for(c) & remaining))
        gain = pairs_for(best) & remaining
        if not gain:
            break
        selected.append(best)
        remaining -= gain
        candidates.remove(best)

    return selected


def sql_literal_json(obj: dict) -> str:
    text = json.dumps(obj, ensure_ascii=False, sort_keys=True)
    return "$ppjson$" + text + "$ppjson$::jsonb"


def sql_literal_text(s: str) -> str:
    return "$ppq$" + s + "$ppq$"


def emit_sql(cases: Sequence[Case], mode: str) -> str:
    rows = []
    for c in cases:
        rows.append(
            "  ("
            + ", ".join(
                [
                    sql_literal_text(c.name),
                    sql_literal_json(c.spec()),
                    "true" if c.cap_small else "false",
                    sql_literal_text(c.rel),
                    sql_literal_text(c.hypoform),
                ]
            )
            + ")"
        )

    values_sql = ",\n".join(rows)
    expected_count = len(cases)

    return f"""-- generated by tools/gen_pathprobe_tests.py --mode {mode}
-- Do not compare exact planner costs here.  This test asserts JSON invariants.
LOAD 'pathprobe';

CREATE TABLE combo (id int, v int);
INSERT INTO combo SELECT g, g % 100 FROM generate_series(1, 2200) g;
ANALYZE combo;
CREATE INDEX combo_id ON combo (id);

CREATE TEMP TABLE pp_combo_cases(
  name text PRIMARY KEY,
  spec jsonb NOT NULL,
  cap_small boolean NOT NULL,
  rel text NOT NULL,
  hypoform text NOT NULL
);  -- session-lifetime temp: pg_regress runs autocommit, ON COMMIT DROP would kill it before INSERT

INSERT INTO pp_combo_cases(name, spec, cap_small, rel, hypoform)
VALUES
{values_sql};

DO $body$
DECLARE
  r          record;
  j          jsonb;
  c          jsonb;
  h          jsonb;
  ncases     int := 0;
  nfail      int := 0;
  fate       text;
  msg        text;
  naccepted  int;
  nrejected  int;
  nsurvived  int;
BEGIN
  FOR r IN SELECT * FROM pp_combo_cases ORDER BY name
  LOOP
    ncases := ncases + 1;
    PERFORM set_config('pathprobe.max_events',
                       CASE WHEN r.cap_small THEN '16' ELSE '10000' END, true);

    BEGIN
      j := pathprobe_propose_json({sql_literal_text(QUERY)}, r.spec::text)::jsonb;
    EXCEPTION WHEN OTHERS THEN
      nfail := nfail + 1;
      RAISE NOTICE 'case % (%): call failed: %', ncases, r.name, SQLERRM;
      CONTINUE;
    END;

    msg := NULL;

    -- I1: candidates[] present with exactly one entry.
    IF coalesce(jsonb_typeof(j->'candidates'), '') <> 'array' THEN
      msg := 'candidates missing or not array';
    ELSIF jsonb_array_length(j->'candidates') <> 1 THEN
      msg := 'candidates length';
    END IF;

    IF msg IS NULL THEN
      c := j->'candidates'->0;
      fate := c->>'fate';
      naccepted := coalesce((c->'event_counts'->>'accepted')::int, 0);
      nrejected := coalesce((c->'event_counts'->>'rejected')::int, 0);
      nsurvived := coalesce((c->'event_counts'->>'survived')::int, 0);
    END IF;

    -- I2: fate from the allowed alphabet.
    IF msg IS NULL AND fate NOT IN
       ('survived','survived_upper','accepted_not_survived',
        'rejected','not_injected','unknown','inconsistent') THEN
      msg := 'fate alphabet: ' || coalesce(fate, '<null>');
    END IF;

    -- I3: injected iff target relation is in the query.
    IF msg IS NULL AND coalesce((c->>'injected')::bool, false) <> (r.rel = 'combo') THEN
      msg := 'injected vs target';
    END IF;

    -- I4: not injected => fate not_injected and zero event counts.
    IF msg IS NULL AND NOT coalesce((c->>'injected')::bool, false) AND
       (fate <> 'not_injected' OR naccepted <> 0 OR nrejected <> 0 OR nsurvived <> 0) THEN
      msg := 'not-injected consistency';
    END IF;

    -- I5: survived <= accepted.  Frontier survival needs prior acceptance.
    IF msg IS NULL AND nsurvived > naccepted THEN
      msg := 'survived > accepted';
    END IF;

    -- I6: hypo entries: in_plan -> injected; survived <= accepted.
    IF msg IS NULL AND coalesce(jsonb_typeof(j->'hypo_indexes'), 'array') <> 'array' THEN
      msg := 'hypo_indexes not array';
    END IF;

    IF msg IS NULL THEN
      FOR h IN SELECT * FROM jsonb_array_elements(coalesce(j->'hypo_indexes','[]'::jsonb))
      LOOP
        IF coalesce((h->>'in_plan')::bool, false) AND NOT coalesce((h->>'injected')::bool, false) THEN
          msg := 'hypo in_plan without injected';
          EXIT;
        END IF;
        IF coalesce((h->>'survived')::int, 0) > coalesce((h->>'accepted')::int, 0) THEN
          msg := 'hypo survived > accepted';
          EXIT;
        END IF;
      END LOOP;
    END IF;

    -- I7: advise => advisor block enabled with baseline/plan costs.
    IF msg IS NULL AND r.hypoform = 'advise' THEN
      IF coalesce(jsonb_typeof(j->'advisor'), '') <> 'object'
         OR NOT coalesce((j->'advisor'->>'enabled')::bool, false)
         OR NOT (j->'advisor' ? 'baseline_cost')
         OR NOT (j->'advisor' ? 'plan_cost') THEN
        msg := 'advisor block incomplete';
      END IF;
    END IF;

    -- I8: limits block present; truncated implies capped length.
    IF msg IS NULL THEN
      IF coalesce(jsonb_typeof(j->'limits'), '') <> 'object' THEN
        msg := 'limits block missing';
      ELSIF coalesce((j->'limits'->>'events_truncated')::bool, false)
            AND r.cap_small
            AND jsonb_array_length(coalesce(j->'events','[]'::jsonb)) > 16 THEN
        msg := 'truncated but over cap';
      END IF;
    END IF;

    -- I9: seq strictly monotone over emitted event order.
    IF msg IS NULL THEN
      PERFORM 1
      FROM (
        SELECT ord,
               (e->>'seq')::int AS s,
               lag((e->>'seq')::int) OVER (ORDER BY ord) AS p
        FROM jsonb_array_elements(coalesce(j->'events','[]'::jsonb))
             WITH ORDINALITY AS x(e, ord)
      ) q
      WHERE p IS NOT NULL AND s <= p;
      IF FOUND THEN
        msg := 'seq not monotone';
      END IF;
    END IF;

    IF msg IS NOT NULL THEN
      nfail := nfail + 1;
      RAISE NOTICE 'case % (%): % spec=%', ncases, r.name, msg, r.spec;
    END IF;
  END LOOP;

  RAISE NOTICE 'combinatorial generated {mode}: % cases, % failures', ncases, nfail;
  IF ncases <> {expected_count} THEN
    RAISE EXCEPTION 'expected {expected_count} cases, got %', ncases;
  END IF;
  IF nfail > 0 THEN
    RAISE EXCEPTION 'combinatorial generated invariants failed';
  END IF;
END;
$body$;

DROP TABLE combo;
"""


def emit_expected(cases: Sequence[Case], mode: str) -> str:
    """Return a plain PostgreSQL regression expected file.

    The standard pg_regress output echoes SQL input and includes NOTICE lines.
    Since our generated SQL emits exactly one success NOTICE, derive expected
    from the SQL text and insert that NOTICE before the final DROP TABLE.
    """
    sql = emit_sql(cases, mode)
    marker = "DROP TABLE combo;\n"
    notice = f"NOTICE:  combinatorial generated {mode}: {len(cases)} cases, 0 failures\n"
    if marker not in sql:
        raise AssertionError("DROP TABLE marker not found in generated SQL")
    return sql.replace(marker, notice + marker)


def emit_matrix(cases: Sequence[Case], mode: str) -> str:
    lines = [
        f"# Generated pathprobe matrix ({mode})",
        "",
        f"Cases: {len(cases)}",
        "",
        "| case | rel | forbid | cheap | ordered | hypoform | cap |",
        "|---|---|---:|---:|---:|---|---|",
    ]
    for c in cases:
        lines.append(
            f"| `{c.name}` | `{c.rel}` | {str(c.forbid).lower()} | "
            f"{str(c.cheap).lower()} | {str(c.ordered).lower()} | "
            f"`{c.hypoform}` | `{c.cap}` |"
        )
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["smoke", "pairwise", "full"], default="smoke")
    ap.add_argument("--sql-out")
    ap.add_argument("--expected-out")
    ap.add_argument("--matrix-out")
    ap.add_argument("--list-cases", action="store_true")
    args = ap.parse_args()

    if args.mode == "smoke":
        cases = smoke_cases()
    elif args.mode == "pairwise":
        cases = pairwise_cases()
    else:
        cases = all_cases()

    if args.list_cases:
        for c in cases:
            print(c.name)
        return

    sql = emit_sql(cases, args.mode)
    if args.sql_out:
        Path(args.sql_out).write_text(sql, encoding="utf-8")
    else:
        print(sql)

    if args.expected_out:
        Path(args.expected_out).write_text(emit_expected(cases, args.mode), encoding="utf-8")

    if args.matrix_out:
        Path(args.matrix_out).write_text(emit_matrix(cases, args.mode), encoding="utf-8")


if __name__ == "__main__":
    main()
