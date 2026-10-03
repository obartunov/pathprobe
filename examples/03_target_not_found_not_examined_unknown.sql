-- Example 3: missing observation is not a planner state.
-- target_not_found, not_examined and unknown all show zero events; they
-- mean different things.
--   psql -X -f examples/03_target_not_found_not_examined_unknown.sql
-- See docs/examples/03-target-not-found-not-examined-unknown.md.

\ir _common.sql

DROP TABLE IF EXISTS ex3_orders, ex3_customers;
CREATE TABLE ex3_customers (id int PRIMARY KEY, region int);
CREATE TABLE ex3_orders (id int PRIMARY KEY, customer_id int, amount int);
INSERT INTO ex3_customers SELECT g, g % 10 FROM generate_series(1, 1000) g;
INSERT INTO ex3_orders SELECT g, g % 1000 + 1, g % 97 FROM generate_series(1, 20000) g;
ANALYZE ex3_customers, ex3_orders;

-- One proposed candidate, deliberately more expensive than the primary key
-- scan, aimed at three different targets.
CREATE FUNCTION pg_temp.ex3_probe(q text, target text) RETURNS jsonb
LANGUAGE sql AS $$
  SELECT pathprobe_propose_json(q, format(
    '{"target":{"relname":"%s"},"candidate":{"pathname":"probe","provider":"ex"},'
    '"cost":{"startup_cost":500,"total_cost":500,"rows":1},'
    '"planner":{"parallel_safe":false}}', target))::jsonb
$$;

-- The LEFT JOIN to ex3_customers is removed by join removal (id is unique
-- and no column of c is used), so ex3_customers never reaches path
-- generation.
EXPLAIN (COSTS OFF)
SELECT o.* FROM ex3_orders o LEFT JOIN ex3_customers c ON c.id = o.customer_id WHERE o.id = 7;

SELECT n, target, j->'candidates'->0->>'fate' AS fate,
       j->'candidates'->0->>'injected' AS injected,
       j->'candidates'->0->'event_counts' AS event_counts
FROM (VALUES
  (1, 'nosuchtable',   'SELECT * FROM ex3_orders WHERE id = 7'),
  (2, 'ex3_customers', 'SELECT o.* FROM ex3_orders o LEFT JOIN ex3_customers c ON c.id = o.customer_id WHERE o.id = 7'),
  (3, 'ex3_orders',    'SELECT * FROM ex3_orders WHERE id = 7')) v(n, target, q),
LATERAL pg_temp.ex3_probe(q, target) j
ORDER BY n;

-- The same rejected candidate when rejections are not observed: with
-- pathprobe.min_gap set, the rejected hook drops events whose gap is
-- smaller than min_gap, and coverage says so.
SET pathprobe.min_gap = 1000000;
SELECT j->'coverage'->>'add_path_rejected' AS rejection_covered,
       j->'candidates'->0->>'fate' AS fate,
       j->'candidates'->0->'event_counts' AS event_counts
FROM pg_temp.ex3_probe('SELECT * FROM ex3_orders WHERE id = 7', 'ex3_orders') j;
RESET pathprobe.min_gap;

-- Structural checks.
WITH r AS (
  SELECT n, j FROM (VALUES
    (1, 'nosuchtable',   'SELECT * FROM ex3_orders WHERE id = 7'),
    (2, 'ex3_customers', 'SELECT o.* FROM ex3_orders o LEFT JOIN ex3_customers c ON c.id = o.customer_id WHERE o.id = 7'),
    (3, 'ex3_orders',    'SELECT * FROM ex3_orders WHERE id = 7')) v(n, target, q),
  LATERAL pg_temp.ex3_probe(q, target) j)
SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('no such relation: target_not_found',
   (SELECT j->'candidates'->0->>'fate' = 'target_not_found' FROM r WHERE n = 1)),
  ('relation removed before planning: not_examined',
   (SELECT j->'candidates'->0->>'fate' = 'not_examined' FROM r WHERE n = 2)),
  ('both carry no events at all',
   (SELECT bool_and((SELECT sum(v::int) FROM jsonb_each_text(j->'candidates'->0->'event_counts') x(k, v)) = 0)
    FROM r WHERE n IN (1, 2))),
  ('with full coverage the candidate is rejected',
   (SELECT j->'candidates'->0->>'fate' = 'rejected'
           AND (j->'coverage'->>'add_path_rejected')::bool FROM r WHERE n = 3))) v(label, ok);

SET pathprobe.min_gap = 1000000;
SELECT pg_temp.ex_check('rejection not covered: the same candidate is unknown, not rejected',
         j->'candidates'->0->>'fate' = 'unknown'
         AND NOT (j->'coverage'->>'add_path_rejected')::bool) AS "check"
FROM pg_temp.ex3_probe('SELECT * FROM ex3_orders WHERE id = 7', 'ex3_orders') j;
RESET pathprobe.min_gap;

DROP TABLE ex3_orders, ex3_customers;
