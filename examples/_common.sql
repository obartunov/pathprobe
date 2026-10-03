-- Shared setup for the examples, included with \ir.  Needs a superuser
-- (LOAD, pathprobe.log_accepted, pathprobe.min_gap and hypo_sql are
-- superuser-only) and a server built with patches/pathprobe-core-hooks.patch.

\set ON_ERROR_STOP on
-- warnings only: hides the NOTICEs of DROP TABLE IF EXISTS on a first run
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
LOAD 'pathprobe';

-- Fail the script when a structural expectation does not hold.
CREATE FUNCTION pg_temp.ex_check(label text, ok bool) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
  IF ok IS NOT TRUE THEN
    RAISE EXCEPTION 'example check failed: %', label;
  END IF;
  RETURN 'ok: ' || label;
END $$;

-- Top plan node type of EXPLAIN, for checks on what EXPLAIN shows.
CREATE FUNCTION pg_temp.ex_plan_node(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  p json;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || q INTO p;
  RETURN p->0->'Plan'->>'Node Type';
END $$;

-- Deterministic plans: no parallel paths.
SET max_parallel_workers_per_gather = 0;
