"""
pathprobe model-based test definitions.

This module is the *source of truth* for the model-based test layer.  It
declares the model dimensions, the fixtures the model runs against, and the
explicit case set with per-case oracles.  The runner (pathprobe_model_runner.py)
executes these cases and checks the declared expectations; no generated SQL file
is authoritative.

Two independent models:

  security     - ACL / RLS / view / spec boundary invariants
  correctness  - path / proposed / hypo / advisor lifecycle invariants

Oracles
  security:    OK, DENIED, NO_LEAK, NO_GUC_LEAK  (a case may declare several)
  correctness: allowed_fates + structural JSON invariants (checked in runner)

NO_LEAK semantics (important):
  A DENIED result is not automatically leak-free: a permission error can still
  name a relation or column the caller never referenced (e.g. the base table of
  a view).  Each case carries `forbidden_names` = names that must NOT appear in
  the returned text/JSON *or* the error message.  Names the caller itself put in
  the query/spec are not leaks and are not listed.
"""

from dataclasses import dataclass, field
from typing import List, Optional, Set


# --------------------------------------------------------------------------
# Dimensions (kept as plain strings so --emit-matrix and --list-cases are
# readable and the model stays diffable).
# --------------------------------------------------------------------------

ROLES = [
    "superuser",
    "table_owner",
    "agent_table_select",
    "agent_column_select",
    "agent_no_select",
    "agent_no_schema_usage",
    "intruder",
]

RELATIONS = [
    "allowed_table",
    "private_table",
    "column_restricted_table",
    "rls_table",
    "view_over_allowed_table",
    "view_over_private_table",
]

QUERY_SHAPES = [
    "top_select",
    "cte",
    "subquery",
    "exists_sublink",
    "union_branch",
    "join",
    "multi_statement",
    "dml_or_modifying_cte",
]

SPEC_SHAPES = [
    "none",
    "candidate_allowed_rel",
    "candidate_private_rel",
    "structured_hypo_allowed_column",
    "structured_hypo_forbidden_column",
    "structured_hypo_mixed_columns",
    "hypo_sql",
    "advisor_true",
    "hints_valid",
    "hints_malformed",
]

# correctness dimensions
TARGETS = ["target_exists", "target_missing"]
CANDIDATE_COSTS = ["cheap", "expensive"]
POLICIES = ["winner_forbidden_true", "winner_forbidden_false"]
PATHKEYS = ["claimed", "not_claimed"]
PARALLELISM = ["pinned_off", "default", "forced_on"]
ADVISORS = ["disabled", "enabled"]
HYPOS = ["none", "structured_btree", "structured_forbidden", "hypo_sql"]

# fate alphabet (authoritative: pathprobe_propose.c fate rendering)
FATE_ALPHABET = {
    "not_injected",
    "survived",
    "survived_upper",
    "accepted_not_survived",
    "rejected",
    "unknown",
}


# --------------------------------------------------------------------------
# Fixtures.  The runner installs these once as superuser; role names and
# relation names here MUST match what the case builders reference.
# --------------------------------------------------------------------------

FIXTURE_ROLES = [
    "ppm_owner",
    "ppm_agent_tbl",
    "ppm_agent_col",
    "ppm_agent_none",
    "ppm_agent_noschema",
    "ppm_intruder",
]

# role dimension -> concrete SQL role (None => RESET ROLE / superuser)
ROLE_SQL = {
    "superuser": None,
    "table_owner": "ppm_owner",
    "agent_table_select": "ppm_agent_tbl",
    "agent_column_select": "ppm_agent_col",
    "agent_no_select": "ppm_agent_none",
    "agent_no_schema_usage": "ppm_agent_noschema",
    "intruder": "ppm_intruder",
}

SETUP_SQL = r"""
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;

DROP SCHEMA IF EXISTS ppm CASCADE;
DROP SCHEMA IF EXISTS ppm_hidden CASCADE;
CREATE SCHEMA ppm;
CREATE SCHEMA ppm_hidden;

CREATE TABLE ppm.allowed (id int, v int);
INSERT INTO ppm.allowed SELECT g, g % 100 FROM generate_series(1, 2000) g;
CREATE TABLE ppm.private (id int, secret text);
INSERT INTO ppm.private SELECT g, 'x' FROM generate_series(1, 500) g;
CREATE TABLE ppm.cols (id int, pub int, secret text);
INSERT INTO ppm.cols SELECT g, g % 10, 'x' FROM generate_series(1, 500) g;
CREATE TABLE ppm.rls (id int, owner name, v int);
INSERT INTO ppm.rls SELECT g, 'ppm_agent_tbl', g FROM generate_series(1, 200) g;
ALTER TABLE ppm.rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY ppm_rls_p ON ppm.rls USING (owner = current_user);
CREATE TABLE ppm.owned (id int, v int);
INSERT INTO ppm.owned SELECT g, g FROM generate_series(1, 300) g;
CREATE VIEW ppm.v_allowed AS SELECT * FROM ppm.allowed;
CREATE VIEW ppm.v_private AS SELECT * FROM ppm.private;
CREATE TABLE ppm_hidden.h (id int);
INSERT INTO ppm_hidden.h SELECT generate_series(1, 50);
ANALYZE ppm.allowed, ppm.private, ppm.cols, ppm.rls, ppm.owned, ppm_hidden.h;

-- roles
DROP ROLE IF EXISTS ppm_owner;
DROP ROLE IF EXISTS ppm_agent_tbl;
DROP ROLE IF EXISTS ppm_agent_col;
DROP ROLE IF EXISTS ppm_agent_none;
DROP ROLE IF EXISTS ppm_agent_noschema;
DROP ROLE IF EXISTS ppm_intruder;
CREATE ROLE ppm_owner LOGIN;
CREATE ROLE ppm_agent_tbl LOGIN;
CREATE ROLE ppm_agent_col LOGIN;
CREATE ROLE ppm_agent_none LOGIN;
CREATE ROLE ppm_agent_noschema LOGIN;
CREATE ROLE ppm_intruder LOGIN;

ALTER TABLE ppm.owned OWNER TO ppm_owner;

GRANT USAGE ON SCHEMA ppm TO ppm_owner, ppm_agent_tbl, ppm_agent_col, ppm_agent_none;
GRANT SELECT ON ppm.allowed TO ppm_agent_tbl;
GRANT SELECT (id, pub) ON ppm.cols TO ppm_agent_col;      -- not secret
GRANT SELECT ON ppm.rls TO ppm_agent_tbl;
GRANT SELECT ON ppm.v_allowed TO ppm_agent_tbl;
GRANT SELECT ON ppm.v_private TO ppm_agent_tbl;            -- view only, not base
-- ppm_agent_noschema: table SELECT but no schema USAGE on ppm_hidden
GRANT SELECT ON ppm_hidden.h TO ppm_agent_noschema;
-- ppm_owner can select its own table
-- ppm_agent_none / ppm_intruder get nothing beyond (maybe) schema USAGE

-- helper: run a pathprobe call, classify result without aborting the session.
CREATE OR REPLACE FUNCTION ppm_call(fn text, q text, spec text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE res text; msg text; st text;
BEGIN
  IF    fn = 'propose_json' THEN res := pathprobe_propose_json(q, spec);
  ELSIF fn = 'propose'      THEN res := pathprobe_propose(q, spec);
  ELSIF fn = 'observe_json' THEN res := pathprobe_json(q);
  ELSE  RAISE EXCEPTION 'ppm_call: unknown fn %', fn;
  END IF;
  RETURN 'OK' || chr(30) || '' || chr(30) || res;
EXCEPTION WHEN OTHERS THEN
  GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT;
  RETURN 'ERR' || chr(30) || st || chr(30) || msg;
END $$;
GRANT EXECUTE ON FUNCTION ppm_call(text, text, text) TO PUBLIC;
RESET client_min_messages;
"""

TEARDOWN_SQL = r"""
SET client_min_messages = warning;
DROP FUNCTION IF EXISTS ppm_call(text, text, text);
DROP OWNED BY ppm_owner, ppm_agent_tbl, ppm_agent_col, ppm_agent_none,
              ppm_agent_noschema, ppm_intruder;
DROP SCHEMA IF EXISTS ppm CASCADE;
DROP SCHEMA IF EXISTS ppm_hidden CASCADE;
DROP ROLE IF EXISTS ppm_owner;
DROP ROLE IF EXISTS ppm_agent_tbl;
DROP ROLE IF EXISTS ppm_agent_col;
DROP ROLE IF EXISTS ppm_agent_none;
DROP ROLE IF EXISTS ppm_agent_noschema;
DROP ROLE IF EXISTS ppm_intruder;
RESET client_min_messages;
"""


# --------------------------------------------------------------------------
# Case data classes
# --------------------------------------------------------------------------

@dataclass
class SecurityCase:
    cid: str
    role: str                      # dimension name (see ROLES)
    query: str
    spec: str                      # JSON spec text (may be "" for observe fn)
    fn: str = "propose_json"       # ppm_call function selector
    expect: Set[str] = field(default_factory=set)   # subset of {OK,DENIED,NO_LEAK,NO_GUC_LEAK}
    forbidden_names: Set[str] = field(default_factory=set)  # for NO_LEAK
    dims: dict = field(default_factory=dict)
    note: str = ""


@dataclass
class CorrectnessCase:
    cid: str
    query: str
    spec: str
    fn: str = "propose_json"
    parallelism: str = "pinned_off"
    allowed_fates: Optional[Set[str]] = None       # None => do not assert a fate
    require_fields: Set[str] = field(default_factory=set)   # top-level keys required
    advisor_expected: Optional[bool] = None
    max_events: Optional[int] = None               # set GUC to force truncation test
    expect_truncated: Optional[bool] = None
    dims: dict = field(default_factory=dict)
    note: str = ""


# --------------------------------------------------------------------------
# Mandatory security smoke cases (the 15 required by the model spec).
# forbidden_names lists only names the caller did NOT supply themselves.
# --------------------------------------------------------------------------

def mandatory_security_cases() -> List[SecurityCase]:
    C = SecurityCase
    return [
        C("sec_allowed_ok", "agent_table_select",
          "SELECT * FROM ppm.allowed WHERE v>5", '{"advise":true}',
          expect={"OK"},
          dims={"relation": "allowed_table", "query_shape": "top_select",
                "spec_shape": "advisor_true"},
          note="1. allowed table -> OK"),

        C("sec_private_top", "agent_table_select",
          "SELECT * FROM ppm.private", '{"advise":true}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names=set(),
          dims={"relation": "private_table", "query_shape": "top_select",
                "spec_shape": "advisor_true"},
          note="2. private table top-level -> DENIED (+NO_LEAK: no hidden name)"),

        C("sec_private_cte", "agent_table_select",
          "WITH c AS (SELECT * FROM ppm.private) SELECT * FROM c",
          '{"advise":true}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names=set(),
          dims={"relation": "private_table", "query_shape": "cte",
                "spec_shape": "advisor_true"},
          note="3. private in CTE -> DENIED (walker descends cteList)"),

        C("sec_private_subquery", "agent_table_select",
          "SELECT * FROM (SELECT * FROM ppm.private) s", '{"advise":true}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names=set(),
          dims={"relation": "private_table", "query_shape": "subquery",
                "spec_shape": "advisor_true"},
          note="4. private in subquery -> DENIED"),

        C("sec_private_sublink", "agent_table_select",
          "SELECT * FROM ppm.allowed a WHERE EXISTS "
          "(SELECT 1 FROM ppm.private p WHERE p.id=a.id)", '{"advise":true}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names=set(),
          dims={"relation": "private_table", "query_shape": "exists_sublink",
                "spec_shape": "advisor_true"},
          note="5. private in EXISTS sublink -> DENIED"),

        C("sec_private_union", "agent_table_select",
          "SELECT id FROM ppm.allowed UNION SELECT id FROM ppm.private",
          '{"advise":true}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names=set(),
          dims={"relation": "private_table", "query_shape": "union_branch",
                "spec_shape": "advisor_true"},
          note="6. private in UNION branch -> DENIED"),

        C("sec_hypo_allowed_col", "agent_column_select",
          "SELECT id, pub FROM ppm.cols WHERE pub>5",
          '{"hypo_indexes":[{"relname":"cols","columns":["pub"]}]}',
          expect={"OK"},
          dims={"relation": "column_restricted_table", "query_shape": "top_select",
                "spec_shape": "structured_hypo_allowed_column"},
          note="7. structured hypo allowed column -> OK"),

        C("sec_hypo_forbidden_col", "agent_column_select",
          "SELECT id, pub FROM ppm.cols WHERE pub>5",
          '{"hypo_indexes":[{"relname":"cols","columns":["secret"]}]}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names=set(),
          dims={"relation": "column_restricted_table", "query_shape": "top_select",
                "spec_shape": "structured_hypo_forbidden_column"},
          note="8. structured hypo forbidden column -> DENIED "
               "(secret is caller-supplied, so not a hidden leak)"),

        C("sec_hypo_mixed_col", "agent_column_select",
          "SELECT id, pub FROM ppm.cols WHERE pub>5",
          '{"hypo_indexes":[{"relname":"cols","columns":["pub","secret"]}]}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names=set(),
          dims={"relation": "column_restricted_table", "query_shape": "top_select",
                "spec_shape": "structured_hypo_mixed_columns"},
          note="9. structured hypo mixed columns, one forbidden -> DENIED"),

        C("sec_hypo_sql_nonsuper", "agent_table_select",
          "SELECT * FROM ppm.allowed",
          '{"hypo_sql":["CREATE INDEX zz ON ppm.allowed(id)"]}',
          expect={"DENIED"},
          dims={"relation": "allowed_table", "query_shape": "top_select",
                "spec_shape": "hypo_sql"},
          note="10. non-superuser hypo_sql -> DENIED"),

        C("sec_forbidden_malformed_hints", "agent_table_select",
          "SELECT * FROM ppm.private",
          '{"advise":true,"hints":"notanobject"}',
          expect={"DENIED"},
          dims={"relation": "private_table", "query_shape": "top_select",
                "spec_shape": "hints_malformed"},
          note="11. forbidden query + malformed hints -> DENIED, not hints error "
               "(auth before hint processing)"),

        C("sec_denied_hint_no_guc_leak", "agent_table_select",
          "SELECT * FROM ppm.private",
          '{"advise":true,"hints":{"forbid":["seqscan"]}}',
          expect={"DENIED", "NO_GUC_LEAK"},
          dims={"relation": "private_table", "query_shape": "top_select",
                "spec_shape": "hints_valid"},
          note="12. denied hinted probe -> NO_GUC_LEAK"),

        C("sec_rls_nonsuper", "agent_table_select",
          "SELECT * FROM ppm.rls", '{"advise":true}',
          expect={"DENIED"},
          dims={"relation": "rls_table", "query_shape": "top_select",
                "spec_shape": "advisor_true"},
          note="13. RLS table as non-superuser -> DENIED"),

        C("sec_view_over_private", "agent_table_select",
          "SELECT * FROM ppm.v_private", '{"advise":true}',
          expect={"DENIED", "NO_LEAK"}, forbidden_names={"ppm.private", "private"},
          dims={"relation": "view_over_private_table", "query_shape": "top_select",
                "spec_shape": "advisor_true"},
          note="14. view/rule checkAsUser exposure -> DENIED, base name not leaked"),

        C("sec_superuser_full", "superuser",
          "SELECT * FROM ppm.private",
          '{"hypo_sql":["CREATE INDEX ww ON ppm.private(id)"]}',
          expect={"OK"},
          dims={"relation": "private_table", "query_shape": "top_select",
                "spec_shape": "hypo_sql"},
          note="15. superuser full behavior -> OK"),
    ]


# --------------------------------------------------------------------------
# Mandatory correctness smoke cases (the 10 required).
# All pin parallelism unless the case is specifically about parallelism.
# --------------------------------------------------------------------------

def _cand_spec(total_cost, rows=1, winner_forbidden=False, relname="allowed",
               pathname="c", provider="model", pathkeys=None, parallel_safe=None):
    import json
    spec = {
        "target": {"relname": relname},
        "candidate": {"pathname": pathname, "provider": provider},
        "cost": {"total_cost": total_cost, "rows": rows},
    }
    if winner_forbidden:
        spec["mode"] = {"winner_forbidden": True}
    planner = {}
    if pathkeys:
        planner["pathkeys"] = pathkeys
    if parallel_safe is not None:
        planner["parallel_safe"] = parallel_safe
    if planner:
        spec["planner"] = planner
    return json.dumps(spec)


def mandatory_correctness_cases() -> List[CorrectnessCase]:
    C = CorrectnessCase
    return [
        C("cor_target_missing",
          "SELECT * FROM ppm.allowed WHERE v>5",
          _cand_spec(0.5, relname="nosuchtable"),
          allowed_fates={"not_injected"},
          require_fields={"candidates", "events"},
          dims={"target": "target_missing", "policy": "winner_forbidden_false"},
          note="1. target missing -> not_injected, zero event counts"),

        C("cor_cheap_injected",
          "SELECT * FROM ppm.allowed WHERE id=42",
          _cand_spec(0.5, parallel_safe=False),
          allowed_fates={"survived", "survived_upper", "accepted_not_survived"},
          require_fields={"candidates", "events"},
          dims={"target": "target_exists", "candidate_cost": "cheap"},
          note="2. cheap candidate, target exists -> injected"),

        C("cor_expensive_candidate",
          "SELECT * FROM ppm.allowed WHERE id=42",
          _cand_spec(1e9, parallel_safe=False),
          allowed_fates={"rejected", "accepted_not_survived"},
          require_fields={"candidates", "events"},
          dims={"target": "target_exists", "candidate_cost": "expensive"},
          note="3. expensive candidate -> rejected or accepted_not_survived"),

        C("cor_winner_forbidden_pinned",
          "SELECT * FROM ppm.allowed WHERE id=42",
          _cand_spec(0.5, winner_forbidden=True),
          parallelism="pinned_off",
          allowed_fates={"survived_upper"},
          require_fields={"candidates"},
          dims={"policy": "winner_forbidden_true", "parallelism": "pinned_off"},
          note="4. winner_forbidden + pinned parallelism -> deterministic survived_upper"),

        C("cor_pathkeys_claimed",
          "SELECT * FROM ppm.allowed ORDER BY id",
          _cand_spec(0.5, pathkeys=["id"]),
          allowed_fates=None,
          require_fields={"candidates", "events"},
          dims={"pathkeys": "claimed"},
          note="5. pathkeys claimed -> pathkeys fields present (structural)"),

        C("cor_advisor_block",
          "SELECT * FROM ppm.allowed WHERE id=42 AND v=3",
          '{"advise":true}',
          allowed_fates=None,
          require_fields={"advisor"},
          advisor_expected=True,
          dims={"advisor": "enabled"},
          note="6. advisor enabled -> advisor block complete"),

        C("cor_hypo_consistency",
          "SELECT * FROM ppm.allowed WHERE id=42",
          '{"hypo_indexes":[{"relname":"allowed","columns":["id"]}]}',
          allowed_fates=None,
          require_fields={"hypo_indexes"},
          dims={"hypo": "structured_btree"},
          note="7. hypo in_plan -> injected/survived/accepted consistency"),

        C("cor_truncation",
          "SELECT * FROM ppm.allowed a JOIN ppm.allowed b ON a.id=b.id "
          "JOIN ppm.allowed c ON b.id=c.id JOIN ppm.allowed d ON c.id=d.id",
          '{"advise":true}',
          allowed_fates=None,
          require_fields={"limits"},
          max_events=16,
          expect_truncated=True,
          dims={"limits": "low_max_events"},
          note="8. low max_events -> truncation flag set and cap respected"),

        C("cor_seq_monotonic",
          "SELECT * FROM ppm.allowed WHERE id=42",
          _cand_spec(0.5),
          allowed_fates=None,
          require_fields={"events"},
          dims={"invariant": "seq_monotonic"},
          note="9. seq monotonic over all emitted events (checked in runner)"),

        C("cor_label_origin",
          "SELECT * FROM ppm.allowed WHERE id=42",
          _cand_spec(0.5, provider="model", pathname="mycand"),
          allowed_fates=None,
          require_fields={"candidates", "events"},
          dims={"invariant": "label_origin"},
          note="10. candidate label/origin present where expected"),
    ]


# --------------------------------------------------------------------------
# Rule-based security oracle for GENERATED (pairwise/full) combinations.
# Returns a set of expectations, or None to EXCLUDE the combination as
# not-well-defined for this pass (kept honest: we do not assert what the model
# cannot justify).
# --------------------------------------------------------------------------

# relations for which a non-superuser role has a usable read path.
# Views are deliberately excluded: pathprobe fails closed on any relation
# reached through another role's privileges (checkAsUser != caller), so a view
# is DENIED for non-superusers even when the caller can read the base.  RLS is
# excluded for the same fail-closed reason.
_ROLE_READABLE = {
    "agent_table_select": {"allowed_table"},
    "agent_column_select": {"column_restricted_table"},  # via id,pub query only
    "agent_no_select": set(),
    "agent_no_schema_usage": set(),
    "intruder": set(),
    "table_owner": {"owned_table"},
}

# columns present on each fixture relation (bases; views mirror their base)
_REL_COLUMNS = {
    "allowed_table": {"id", "v"},
    "private_table": {"id", "secret"},
    "column_restricted_table": {"id", "pub", "secret"},
    "rls_table": {"id", "owner", "v"},
    "view_over_allowed_table": {"id", "v"},
    "view_over_private_table": {"id", "secret"},
}
_IS_VIEW = {"view_over_allowed_table", "view_over_private_table"}


def _spec_valid_on(relation: str, spec_shape: str) -> bool:
    """Whether a spec_shape is structurally valid to attempt on a relation.

    Structurally-invalid combinations are excluded from generation entirely so
    the model never asserts a security outcome for what is really a spec error.
    """
    if spec_shape == "none":
        return False                       # empty spec is not a probe call
    if spec_shape.startswith("structured_hypo"):
        if relation in _IS_VIEW:
            return False                   # cannot build a hypo index on a view
        cols = _REL_COLUMNS[relation]
        if spec_shape == "structured_hypo_allowed_column":
            return "id" in cols
        # forbidden / mixed need a non-selectable 'secret' column to exist
        return "secret" in cols
    return True


def _access_denied(role: str, relation: str, query_shape: str,
                   spec_shape: str) -> bool:
    """Would this probe be denied by the security boundary (ignoring spec
    validity)?  Superuser is never denied on access."""
    if role == "superuser":
        return False
    # non-SELECT / modifying command -> feature_not_supported
    if query_shape in ("multi_statement", "dml_or_modifying_cte"):
        return True
    # hypo_sql (arbitrary DDL) is superuser-only
    if spec_shape == "hypo_sql":
        return True
    # any view / RLS relation referenced by the query -> fail closed
    if relation in _IS_VIEW or relation == "rls_table":
        return True
    # private base referenced by the query
    if relation == "private_table":
        return True
    # structured hypo touching a forbidden (secret) column
    if spec_shape in ("structured_hypo_forbidden_column",
                      "structured_hypo_mixed_columns"):
        return True
    # otherwise the query must be over a relation the role can actually read
    return relation not in _ROLE_READABLE.get(role, set())


def security_oracle(role: str, relation: str, query_shape: str, spec_shape: str):
    """Compute expected oracle for a generated security combination, or None to
    exclude the combination as not-well-defined (kept honest)."""
    if not _spec_valid_on(relation, spec_shape):
        return None

    denied = _access_denied(role, relation, query_shape, spec_shape)

    # malformed hints: auth runs before hint processing, so a forbidden probe
    # is DENIED; on an otherwise-allowed probe it is only a spec error (22023),
    # which is not a security outcome -> exclude.
    if spec_shape == "hints_malformed":
        return {"DENIED"} if denied else None

    if denied:
        exp = {"DENIED"}
        # add NO_LEAK where a hidden (caller-unsupplied) name could surface
        if relation == "view_over_private_table" or spec_shape in (
                "structured_hypo_forbidden_column",
                "structured_hypo_mixed_columns"):
            exp.add("NO_LEAK")
        return exp
    return {"OK"}


def _query_for(relation: str, query_shape: str) -> Optional[str]:
    rel = {
        "allowed_table": "ppm.allowed",
        "private_table": "ppm.private",
        "column_restricted_table": "ppm.cols",
        "rls_table": "ppm.rls",
        "view_over_allowed_table": "ppm.v_allowed",
        "view_over_private_table": "ppm.v_private",
    }[relation]
    cols = "id, pub" if relation == "column_restricted_table" else "*"
    if query_shape == "top_select":
        return f"SELECT {cols} FROM {rel}"
    if query_shape == "cte":
        return f"WITH c AS (SELECT {cols} FROM {rel}) SELECT * FROM c"
    if query_shape == "subquery":
        return f"SELECT * FROM (SELECT {cols} FROM {rel}) s"
    if query_shape == "exists_sublink":
        return (f"SELECT * FROM ppm.allowed a WHERE EXISTS "
                f"(SELECT 1 FROM {rel} r WHERE r.id=a.id)")
    if query_shape == "union_branch":
        return f"SELECT id FROM ppm.allowed UNION SELECT id FROM {rel}"
    if query_shape == "join":
        return f"SELECT a.* FROM ppm.allowed a JOIN {rel} r ON a.id=r.id"
    if query_shape == "multi_statement":
        return f"SELECT {cols} FROM {rel}; SELECT 1"
    if query_shape == "dml_or_modifying_cte":
        return f"UPDATE {rel} SET id=id WHERE false"
    return None


_BARE = {
    "allowed_table": "allowed",
    "private_table": "private",
    "column_restricted_table": "cols",
    "rls_table": "rls",
    "view_over_allowed_table": "v_allowed",
    "view_over_private_table": "v_private",
}


def _spec_for(relation: str, spec_shape: str) -> Optional[str]:
    rel = _BARE[relation]
    if spec_shape == "none":
        return "{}"
    if spec_shape == "advisor_true":
        return '{"advise":true}'
    if spec_shape == "hints_valid":
        return '{"advise":true,"hints":{"forbid":["seqscan"]}}'
    if spec_shape == "hints_malformed":
        return '{"advise":true,"hints":"notanobject"}'
    if spec_shape == "hypo_sql":
        return '{"hypo_sql":["CREATE INDEX zz ON ppm.allowed(id)"]}'
    if spec_shape == "structured_hypo_allowed_column":
        col = "pub" if relation == "column_restricted_table" else "id"
        return '{"hypo_indexes":[{"relname":"%s","columns":["%s"]}]}' % (rel, col)
    if spec_shape == "structured_hypo_forbidden_column":
        return '{"hypo_indexes":[{"relname":"%s","columns":["secret"]}]}' % rel
    if spec_shape == "structured_hypo_mixed_columns":
        allowed_col = "pub" if relation == "column_restricted_table" else "id"
        return ('{"hypo_indexes":[{"relname":"%s","columns":["%s","secret"]}]}'
                % (rel, allowed_col))
    if spec_shape == "candidate_allowed_rel":
        return _cand_spec(0.5, relname="allowed")
    if spec_shape == "candidate_private_rel":
        return _cand_spec(0.5, relname="private")
    return None


def _constraint_skip(role: str, relation: str, query_shape: str,
                     spec_shape: str) -> Optional[str]:
    """Explicit redundancy/validity constraints (with human-readable reasons).

    Returns a reason string if the combination should be skipped before running,
    or None if it should be attempted.  Mirrors the equivalence-class reasoning
    in docs/testing/model-based-tests.md so the model does not enumerate a full
    Cartesian product.
    """
    # structural spec invalidity: empty spec / hypo on a view / missing column
    if not _spec_valid_on(relation, spec_shape):
        if spec_shape == "none":
            return "spec 'none' is not a probe call (target.relname required)"
        return "spec_shape structurally invalid on relation (hypo on view / no such column)"
    # superuser bypasses ACL uniformly: representative superuser cases are kept in
    # smoke and targeted; the generic pairwise fan-out over superuser is redundant
    if role == "superuser":
        return "superuser bypass path is uniform (representatives kept in smoke/targeted)"
    # multi-statement / modifying command is rejected at the raw-parse guard,
    # before any spec or inner-shape handling -> one representative suffices
    if query_shape == "multi_statement" and spec_shape != "advisor_true":
        return "multi_statement rejected at raw-parse guard (one representative kept)"
    if query_shape == "dml_or_modifying_cte" and spec_shape != "advisor_true":
        return "modifying command rejected before spec (one representative kept)"
    # RLS fail-closed precedes any hypo column check
    if relation == "rls_table" and spec_shape.startswith("structured_hypo"):
        return "RLS fail-closed precedes hypo column check"
    # view fail-closed precedes candidate-spec specifics
    if relation in _IS_VIEW and spec_shape in ("candidate_allowed_rel",
                                               "candidate_private_rel"):
        return "view fail-closed precedes candidate spec"
    # malformed hints are covered by allowed/forbidden top_select representatives
    if spec_shape == "hints_malformed" and query_shape != "top_select":
        return "hints_malformed covered by top_select allowed/forbidden representatives"
    return None


def _mk_security_case(role, relation, qshape, sshape, prefix="gen"):
    """Build a SecurityCase for a decided combination, or None if excluded."""
    exp = security_oracle(role, relation, qshape, sshape)
    if exp is None:
        return None, "undecided by conservative oracle"
    q = _query_for(relation, qshape)
    s = _spec_for(relation, sshape)
    if q is None or s is None:
        return None, "no query/spec builder"
    forb = set()
    if relation == "view_over_private_table":
        forb = {"ppm.private", "private"}
    elif sshape in ("structured_hypo_forbidden_column",
                    "structured_hypo_mixed_columns"):
        forb = {"secret"}   # caller-supplied; runner strips, so guards regressions
    cid = "%s_%s__%s__%s__%s" % (prefix, role, relation, qshape, sshape)
    return SecurityCase(cid, role, q, s, expect=exp, forbidden_names=forb,
                        dims={"role": role, "relation": relation,
                              "query_shape": qshape, "spec_shape": sshape}), None


def _pairwise_security():
    """All-pairs over (role, relation, query_shape, spec_shape) with explicit
    constraints applied.  Returns (cases, skips) where skips is a list of
    (dims, reason)."""
    combos = _all_pairs([ROLES, RELATIONS, QUERY_SHAPES, SPEC_SHAPES])
    cases, skips, seen = [], [], set()
    for role, relation, qshape, sshape in combos:
        dims = {"role": role, "relation": relation,
                "query_shape": qshape, "spec_shape": sshape}
        reason = _constraint_skip(role, relation, qshape, sshape)
        if reason:
            skips.append((dims, reason))
            continue
        case, why = _mk_security_case(role, relation, qshape, sshape, "pw")
        if case is None:
            skips.append((dims, why))
            continue
        if case.cid in seen:
            continue
        seen.add(case.cid)
        cases.append(case)
    return cases, skips


# targeted 3-wise interaction groups (security).  Each group fixes the
# non-interacting dimensions to representatives and takes the full product over
# the three interacting dimensions (which is 3-wise coverage for that triple).
_TARGET_GROUPS = {
    # role x query_level x relation_privilege: CTE/subquery/sublink/set-op leaks
    "role_query_relpriv": {
        "roles": ["agent_table_select", "agent_column_select", "intruder"],
        "relations": ["allowed_table", "private_table"],
        "qshapes": ["top_select", "cte", "subquery", "exists_sublink",
                    "union_branch", "join"],
        "sshapes": ["advisor_true"],
    },
    # role x query_allowedness x malformed_hints: hints-before-auth
    "role_allowed_hints": {
        "roles": ["agent_table_select", "superuser"],
        "relations": ["allowed_table", "private_table"],
        "qshapes": ["top_select"],
        "sshapes": ["hints_malformed"],
    },
    # role x structured_hypo x column_privilege: forbidden-column hypo leaks
    "role_hypo_colpriv": {
        "roles": ["agent_column_select", "superuser"],
        "relations": ["column_restricted_table"],
        "qshapes": ["top_select"],
        "sshapes": ["structured_hypo_allowed_column",
                    "structured_hypo_forbidden_column",
                    "structured_hypo_mixed_columns"],
    },
    # advisor x hypo x column_privilege: advisor must not suggest forbidden cols
    "advisor_hypo_colpriv": {
        "roles": ["agent_column_select", "superuser"],
        "relations": ["column_restricted_table"],
        "qshapes": ["top_select"],
        "sshapes": ["advisor_true"],
    },
    # view/rewrite x checkAsUser x non_superuser: view/rule base exposure
    "view_checkasuser": {
        "roles": ["agent_table_select", "agent_column_select"],
        "relations": ["view_over_allowed_table", "view_over_private_table"],
        "qshapes": ["top_select"],
        "sshapes": ["advisor_true"],
    },
}


def _targeted_security():
    import itertools
    cases, skips, seen = [], [], set()
    for gname, g in _TARGET_GROUPS.items():
        for role, relation, qshape, sshape in itertools.product(
                g["roles"], g["relations"], g["qshapes"], g["sshapes"]):
            dims = {"role": role, "relation": relation,
                    "query_shape": qshape, "spec_shape": sshape, "group": gname}
            reason = _constraint_skip(role, relation, qshape, sshape)
            if reason:
                skips.append((dims, reason))
                continue
            case, why = _mk_security_case(role, relation, qshape, sshape,
                                          "t_" + gname)
            if case is None:
                skips.append((dims, why))
                continue
            if case.cid in seen:
                continue
            seen.add(case.cid)
            # advisor group: assert the forbidden column name never surfaces
            if gname == "advisor_hypo_colpriv" and "OK" in case.expect:
                case.expect = set(case.expect) | {"NO_LEAK"}
                case.forbidden_names = {"secret"}
            case.dims["group"] = gname
            cases.append(case)
    return cases, skips


def generate_security_ex(mode: str):
    """Return (cases, skips) for the requested mode."""
    if mode == "smoke":
        return mandatory_security_cases(), []
    if mode == "targeted":
        return _targeted_security()
    if mode == "fuzz":
        cases, skips = _pairwise_security()
        return cases, skips
    # default: pairwise
    return _pairwise_security()


def generate_security(mode: str):
    cases, _ = generate_security_ex(mode)
    return cases


def _all_pairs(dim_lists):
    import itertools
    need = set()
    n = len(dim_lists)
    for i, j in itertools.combinations(range(n), 2):
        for vi in dim_lists[i]:
            for vj in dim_lists[j]:
                need.add((i, vi, j, vj))
    chosen = []
    candidates = list(itertools.product(*dim_lists))
    covered = set()
    while need - covered:
        best, best_gain = None, -1
        for tup in candidates:
            gain = 0
            for i, j in itertools.combinations(range(n), 2):
                key = (i, tup[i], j, tup[j])
                if key in need and key not in covered:
                    gain += 1
            if gain > best_gain:
                best, best_gain = tup, gain
        if best is None or best_gain <= 0:
            break
        chosen.append(best)
        for i, j in itertools.combinations(range(n), 2):
            covered.add((i, best[i], j, best[j]))
    return chosen


def generate_correctness(mode: str):
    if mode == "smoke":
        return mandatory_correctness_cases()
    # targeted/pairwise/fuzz: add the environment-sensitivity matrix
    # winner_forbidden x parallelism x path_competition (the correctness triple
    # that the winner_forbidden regression proved must not assume a single fate).
    out = list(mandatory_correctness_cases())
    for par in PARALLELISM:
        for pol in POLICIES:
            for comp in ("cheap", "expensive"):
                wf = pol == "winner_forbidden_true"
                cost = 0.5 if comp == "cheap" else 1e9
                # Align parallel_safe with the pinned base scan ONLY for the
                # cost-competition arm.  winner_forbidden survival depends on the
                # parallel_safe mismatch, so those keep the default (true).
                psafe = False if (par == "pinned_off" and not wf) else None
                spec = _cand_spec(cost, winner_forbidden=wf, parallel_safe=psafe)
                if wf and par == "pinned_off":
                    fates = {"survived_upper"}
                elif wf:
                    fates = {"survived_upper", "rejected"}
                elif par == "pinned_off" and comp == "cheap":
                    fates = {"survived", "survived_upper", "accepted_not_survived"}
                elif par == "pinned_off" and comp == "expensive":
                    fates = {"rejected", "accepted_not_survived"}
                else:
                    fates = {"survived", "survived_upper",
                             "accepted_not_survived", "rejected"}
                out.append(CorrectnessCase(
                    "gen_par_%s__%s__%s" % (par, pol, comp),
                    "SELECT * FROM ppm.allowed WHERE id=42", spec,
                    parallelism=par, allowed_fates=fates,
                    require_fields={"candidates"},
                    dims={"parallelism": par, "policy": pol, "competition": comp}))
    return out
