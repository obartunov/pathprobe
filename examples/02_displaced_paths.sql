-- Example 2: accepted is not final.  A path enters the pathlist and is
-- later pushed out by a better one.
--   psql -X -f examples/02_displaced_paths.sql
-- See docs/examples/02-displaced-paths.md.

\ir _common.sql

SET pathprobe.log_accepted = on;

DROP TABLE IF EXISTS ex2_orders, ex2_customers;
CREATE TABLE ex2_customers (id int PRIMARY KEY, region int);
CREATE TABLE ex2_orders (id int PRIMARY KEY, customer_id int, amount int);
INSERT INTO ex2_customers SELECT g, g % 10 FROM generate_series(1, 1000) g;
INSERT INTO ex2_orders SELECT g, g % 1000 + 1, g % 97 FROM generate_series(1, 20000) g;
CREATE INDEX ex2_orders_customer_idx ON ex2_orders (customer_id);
ANALYZE ex2_customers, ex2_orders;

\set q 'SELECT * FROM ex2_orders o JOIN ex2_customers c ON c.id = o.customer_id WHERE c.region = 3'

-- 1. Real paths at the join rel: every acceptance and displacement, in
-- planner order.  A DISPLACED event follows the ACCEPTED event of the path
-- that displaced it; gap_cost is old.total_cost - new.total_cost on both.
SELECT (e->>'seq')::int AS seq, e->>'pathtype' AS path, e->>'kind' AS event,
       e->>'gap_cost' AS gap_cost
FROM jsonb_array_elements(pathprobe_json(:'q')::jsonb->'events') e
WHERE e->>'rel' = '{1,2}' AND e->>'kind' IN ('accepted', 'displaced', 'survived')
ORDER BY 1;

-- 2. Proposed candidates have a per-candidate fate.  "early" is in the
-- pathlist right after its own add_path() (survived = 1), and is displaced
-- by "late" afterwards.
SELECT c->>'pathname' AS candidate, c->>'fate' AS fate,
       c->'event_counts' AS event_counts
FROM jsonb_array_elements(pathprobe_propose_json(
  'SELECT * FROM ex2_orders WHERE customer_id = 7',
  '{"candidates":[
     {"target":{"relname":"ex2_orders"},"candidate":{"pathname":"early","provider":"ex"},
      "cost":{"total_cost":30.0,"rows":20},"planner":{"parallel_safe":false}},
     {"target":{"relname":"ex2_orders"},"candidate":{"pathname":"late","provider":"ex"},
      "cost":{"total_cost":1.0,"rows":20},"planner":{"parallel_safe":false}}]}')::jsonb->'candidates') c
ORDER BY 1;

-- Structural checks; no cost values are asserted.
WITH ev AS (SELECT (e->>'seq')::int AS seq, e->>'kind' AS kind,
                   e->>'pathtype' AS path, e->>'gap_cost' AS gap
            FROM jsonb_array_elements(pathprobe_json(:'q')::jsonb->'events') e
            WHERE e->>'rel' = '{1,2}'),
     cand AS (SELECT c FROM jsonb_array_elements(pathprobe_propose_json(
  'SELECT * FROM ex2_orders WHERE customer_id = 7',
  '{"candidates":[
     {"target":{"relname":"ex2_orders"},"candidate":{"pathname":"early","provider":"ex"},
      "cost":{"total_cost":30.0,"rows":20},"planner":{"parallel_safe":false}},
     {"target":{"relname":"ex2_orders"},"candidate":{"pathname":"late","provider":"ex"},
      "cost":{"total_cost":1.0,"rows":20},"planner":{"parallel_safe":false}}]}')::jsonb->'candidates') c)
SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('a merge join path was accepted and later displaced',
   EXISTS (SELECT 1 FROM ev a JOIN ev d ON d.path = a.path AND d.seq > a.seq
           WHERE a.path = 'MergeJoin' AND a.kind = 'accepted' AND d.kind = 'displaced')),
  ('every displacement follows the acceptance of its displacer, same gap_cost',
   (SELECT bool_and(d.gap IS NOT NULL AND d.gap = (SELECT a.gap FROM ev a
                     WHERE a.kind = 'accepted' AND a.seq < d.seq
                     ORDER BY a.seq DESC LIMIT 1))
    FROM ev d WHERE d.kind = 'displaced')),
  ('the surviving join path is a hash join',
   (SELECT bool_and(path = 'HashJoin') FROM ev WHERE kind = 'survived')),
  ('candidate "early" survived its own add_path() but its fate is displaced',
   (SELECT c->>'fate' = 'displaced'
           AND (c->'event_counts'->>'survived')::int = 1
           AND (c->'event_counts'->>'displaced')::int = 1
    FROM cand WHERE c->>'pathname' = 'early')),
  ('candidate "late" survived',
   (SELECT c->>'fate' IN ('survived', 'survived_upper') FROM cand
    WHERE c->>'pathname' = 'late'))) v(label, ok);

DROP TABLE ex2_orders, ex2_customers;
