-- Example 4: injected is not generated; generated is not accepted.
--   psql -X -f examples/04_not_generated_index_path.sql
-- See docs/examples/04-not-generated-index-path.md.

\ir _common.sql

DROP TABLE IF EXISTS ex4_ab;
CREATE TABLE ex4_ab (a int, b int, pad text);
INSERT INTO ex4_ab SELECT g % 1000, g % 50, repeat('x', 40) FROM generate_series(1, 20000) g;
ANALYZE ex4_ab;

\set q 'SELECT * FROM ex4_ab WHERE b = 5'

-- EXPLAIN: a sequential scan.  It cannot say anything about indexes that do
-- not exist, and nothing about why a hypothetical one is not used.
EXPLAIN (COSTS OFF) :q;

-- Three hypothetical indexes for a query that filters only b:
--   ab       btree (a, b)  btree needs no leading-column clause: a path over
--                          (a, b) with b = 5 is built, then loses
--   hash_a   hash (a)      hash requires a clause on its first column
--                          (amoptionalkey = false): no path is built
--   btree_a  btree (a)     no clause, no useful order, no index-only scan:
--                          no path is built
\set spec '{"hypo_indexes":[{"name":"ab","relname":"ex4_ab","columns":["a","b"]},{"name":"btree_a","relname":"ex4_ab","columns":["a"]}],"hypo_sql":["CREATE INDEX hash_a ON ex4_ab USING hash (a)"]}'

SELECT h->>'name' AS hypo, (h->>'injected')::bool AS injected,
       (h->>'accepted')::int AS acc, (h->>'rejected')::int AS rej,
       (h->>'survived')::int AS surv,
       h->>'fate' AS fate,
       h->'index_build'->'calls'->>'plain' AS build_calls,
       h->'index_build'->>'generated' AS generated,
       h->'index_build'->'not_generated' AS not_generated
FROM jsonb_array_elements(pathprobe_propose_json(:'q', :'spec')::jsonb->'hypo_indexes') h
ORDER BY 1;

-- Text form.
SELECT substring(line from '^hypo:\S+') AS hypo, substring(line from 'fate=.*') AS fate
FROM regexp_split_to_table(pathprobe_propose(:'q', :'spec'), E'\n') line
WHERE line LIKE 'hypo:%';

-- Structural checks.
WITH j AS (SELECT pathprobe_propose_json(:'q', :'spec')::jsonb AS j),
     h AS (SELECT x->>'name' AS name, x FROM j, jsonb_array_elements(j->'hypo_indexes') x)
SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('index build decisions are covered',
   (SELECT (j->'coverage'->>'index_build')::bool FROM j)),
  ('all three were injected',
   (SELECT bool_and((x->>'injected')::bool) FROM h)),
  ('hash_a: examined, not generated, no leading clause',
   (SELECT x->>'fate' = 'not_generated'
           AND (x->'index_build'->'calls'->>'plain')::int > 0
           AND (x->'index_build'->>'generated')::int = 0
           AND x->'index_build'->'not_generated' ? 'no_leading_clause'
    FROM h WHERE name = 'hash_a')),
  ('btree_a: examined, not generated, nothing useful',
   (SELECT x->>'fate' = 'not_generated'
           AND x->'index_build'->'not_generated' ? 'no_useful_input'
    FROM h WHERE name = 'btree_a')),
  ('not generated means no add_path() events at all',
   (SELECT bool_and((x->>'accepted')::int + (x->>'rejected')::int
                    + (x->>'survived')::int = 0)
    FROM h WHERE name IN ('hash_a', 'btree_a'))),
  ('ab: generated, then rejected',
   (SELECT x->>'fate' = 'rejected'
           AND (x->'index_build'->>'generated')::int > 0
           AND (x->>'rejected')::int > 0
    FROM h WHERE name = 'ab'))) v(label, ok);

DROP TABLE ex4_ab;
