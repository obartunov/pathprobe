-- excluded: a join family that add_paths_to_joinrel() does not try for an
-- (outer, inner) pair.  Merge and hash join are not tried when their
-- pgs_mask bits are clear, except for a full join, where they are tried as
-- disabled paths where the clauses allow.  A plain nested loop is built
-- whenever the join type allows one; a cleared PGS_NESTLOOP_PLAIN only makes
-- it disabled (the memoize and materialize variants are not built when their
-- bits are clear, but the family as a whole is never excluded).
-- With enable_* settings the mask is the same for every pair, so an
-- excluded family is not tried in that join relation at all.  A hook that
-- sets the mask per pair (pg_plan_advice) can exclude a family for one input
-- order and try it for the other; that case is not covered here.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
SET pathprobe.log_accepted = on;
CREATE TABLE ex_a (id int PRIMARY KEY, k int);
CREATE TABLE ex_b (id int PRIMARY KEY, k int);
INSERT INTO ex_a SELECT i, i % 100 FROM generate_series(1, 2000) i;
INSERT INTO ex_b SELECT i, i % 100 FROM generate_series(1, 2000) i;
ANALYZE ex_a, ex_b;

CREATE FUNCTION ex_ev(q text)
RETURNS TABLE (seq int, rel text, kind text, pathtype text, path_id int,
               disabled_nodes int)
LANGUAGE sql AS $$
  SELECT seq, rel, kind, pathtype, path_id, coalesce(disabled_nodes, 0)
  FROM jsonb_to_recordset(pathprobe_json(q)::jsonb -> 'events')
       AS e(seq int, rel text, kind text, pathtype text, path_id int,
            disabled_nodes int)
$$;

-- per join family at the join relation (the only rel with two members;
-- a full join's relids also hold its outer-join relid): whether it was
-- excluded, whether it was tried (offered to add_path() or cut by its
-- precheck), and whether a tried path was disabled.  Booleans, not counts:
-- the number of attempts depends on the cost model.
CREATE FUNCTION ex_families(q text)
RETURNS TABLE (pathtype text, excluded bool, tried bool, disabled bool)
LANGUAGE sql AS $$
  SELECT t, coalesce(bool_or(kind = 'excluded'), false),
         coalesce(bool_or(kind IN ('accepted', 'rejected', 'skipped')), false),
         coalesce(bool_or(kind IN ('accepted', 'rejected', 'skipped')
                          AND disabled_nodes > 0), false)
  FROM (VALUES ('NestLoop'), ('MergeJoin'), ('HashJoin')) f(t)
  LEFT JOIN ex_ev(q) e ON e.pathtype = f.t AND e.rel LIKE '%,%'
  GROUP BY t ORDER BY t
$$;

-- the invariant, checked after each case: no (rel, family) is both
-- excluded and tried
CREATE FUNCTION ex_contradictions(q text) RETURNS bigint
LANGUAGE sql AS $$
  WITH ev AS (SELECT * FROM ex_ev(q))
  SELECT count(*) FROM ev x
  WHERE x.kind = 'excluded'
    AND EXISTS (SELECT 1 FROM ev e
                WHERE e.rel = x.rel AND e.pathtype = x.pathtype
                  AND e.kind IN ('accepted', 'rejected', 'skipped'))
$$;

-- 1. defaults: nothing is excluded
SELECT * FROM ex_families('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k');
SELECT ex_contradictions('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k') AS contradictions;

-- 2. enable_nestloop = off: nested loops are built as disabled paths, not
--    excluded
SET enable_nestloop = off;
SELECT * FROM ex_families('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k');
SELECT ex_contradictions('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k') AS contradictions;
RESET enable_nestloop;

-- 3. all three off: merge and hash join are excluded, the nested loop is
--    built and is the plan, a disabled one
SET enable_nestloop = off;
SET enable_mergejoin = off;
SET enable_hashjoin = off;
SELECT * FROM ex_families('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k');
SELECT ex_contradictions('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k') AS contradictions;
EXPLAIN (COSTS OFF) SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k;
RESET enable_nestloop;
RESET enable_mergejoin;
RESET enable_hashjoin;

-- 4. enable_hashjoin = off: hash join excluded, and no hash join path
SET enable_hashjoin = off;
SELECT * FROM ex_families('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k');
SELECT ex_contradictions('SELECT * FROM ex_a a JOIN ex_b b ON a.k = b.k') AS contradictions;
RESET enable_hashjoin;

-- 5. full join with merge and hash off: both are still built (disabled);
--    nothing is excluded
SET enable_mergejoin = off;
SET enable_hashjoin = off;
SELECT * FROM ex_families('SELECT * FROM ex_a a FULL JOIN ex_b b ON a.k = b.k');
SELECT ex_contradictions('SELECT * FROM ex_a a FULL JOIN ex_b b ON a.k = b.k') AS contradictions;
RESET enable_mergejoin;
RESET enable_hashjoin;

DROP FUNCTION ex_contradictions(text);
DROP FUNCTION ex_families(text);
DROP FUNCTION ex_ev(text);
DROP TABLE ex_a, ex_b;
