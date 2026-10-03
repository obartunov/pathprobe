-- Example 1: EXPLAIN shows the survivor; pathprobe shows the competition.
--   psql -X -f examples/01_explain_is_not_enough.sql
-- See docs/examples/01-explain-is-not-enough.md.

\ir _common.sql

-- record add_path() acceptances and displacements, not only rejections
SET pathprobe.log_accepted = on;

DROP TABLE IF EXISTS ex1_orders, ex1_customers;
CREATE TABLE ex1_customers (id int PRIMARY KEY, region int);
CREATE TABLE ex1_orders (id int PRIMARY KEY, customer_id int, amount int);
INSERT INTO ex1_customers SELECT g, g % 10 FROM generate_series(1, 1000) g;
INSERT INTO ex1_orders SELECT g, g % 1000 + 1, g % 97 FROM generate_series(1, 20000) g;
CREATE INDEX ex1_orders_customer_idx ON ex1_orders (customer_id);
ANALYZE ex1_customers, ex1_orders;

\set q 'SELECT * FROM ex1_orders o JOIN ex1_customers c ON c.id = o.customer_id WHERE c.region = 3'

-- What EXPLAIN shows: the plan that won.
EXPLAIN (COSTS OFF) :q;

-- What pathprobe shows: the events of the competition, per relation and
-- strategy.  {1} = o, {2} = c, {1,2} = the join.
SELECT e->>'rel' AS rel, e->>'pathtype' AS strategy, e->>'kind' AS event,
       count(*) AS n
FROM jsonb_array_elements(pathprobe_json(:'q')::jsonb->'events') e
WHERE e->>'rel' <> '?'
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3;

-- Same query with merge join disabled: the strategy is excluded before any
-- merge join path is built.
SET enable_mergejoin = off;
SELECT e->>'pathtype' AS strategy, e->>'kind' AS event, count(*) AS n
FROM jsonb_array_elements(pathprobe_json(:'q')::jsonb->'events') e
WHERE e->>'rel' = '{1,2}' AND e->>'kind' IN ('excluded', 'skipped')
GROUP BY 1, 2
ORDER BY 1, 2;
RESET enable_mergejoin;

-- Structural checks; no cost values are asserted.
WITH ev AS (SELECT e FROM jsonb_array_elements(pathprobe_json(:'q')::jsonb->'events') e
            WHERE e->>'rel' = '{1,2}')
SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('EXPLAIN shows a hash join',
   pg_temp.ex_plan_node(:'q') = 'Hash Join'),
  ('the join rel saw all three join strategies',
   (SELECT count(DISTINCT e->>'pathtype') FROM ev) = 3),
  ('merge and nested loop joins were accepted, then displaced',
   (SELECT count(*) FROM ev WHERE e->>'kind' = 'displaced'
      AND e->>'pathtype' IN ('MergeJoin', 'NestLoop')) >= 2),
  ('some join paths were rejected by add_path()',
   (SELECT count(*) FROM ev WHERE e->>'kind' = 'rejected') > 0),
  ('some join paths were skipped before they were built',
   (SELECT count(*) FROM ev WHERE e->>'kind' = 'skipped') > 0),
  ('only hash join paths survived at the join rel',
   (SELECT bool_and(e->>'pathtype' = 'HashJoin') FROM ev
    WHERE e->>'kind' = 'survived'))) v(label, ok);

SET enable_mergejoin = off;
SELECT pg_temp.ex_check('disabled merge join is reported as excluded',
         EXISTS (SELECT 1 FROM jsonb_array_elements(pathprobe_json(:'q')::jsonb->'events') e
                 WHERE e->>'kind' = 'excluded' AND e->>'pathtype' = 'MergeJoin')) AS "check";
RESET enable_mergejoin;

DROP TABLE ex1_orders, ex1_customers;
