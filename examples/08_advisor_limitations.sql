-- Example 8: what the advisor does not propose, and what a surviving
-- hypothesis does not prove.
--   psql -X -f examples/08_advisor_limitations.sql
-- See docs/examples/08-advisor-limitations.md.

\ir _common.sql

DROP TABLE IF EXISTS ex8_orders, ex8_customers;
CREATE TABLE ex8_customers (id int PRIMARY KEY, email text, region int);
CREATE TABLE ex8_orders (id int PRIMARY KEY, customer_id int, status int, total int);
INSERT INTO ex8_customers SELECT g, 'u' || g || '@x', g % 20 FROM generate_series(1, 5000) g;
INSERT INTO ex8_orders SELECT g, g % 5000 + 1, g % 5, g % 1000 FROM generate_series(1, 30000) g;
CREATE INDEX ex8_orders_status_idx ON ex8_orders (status);
ANALYZE ex8_customers, ex8_orders;

-- What the advisor generated for five query shapes.  "atoms" is what it
-- extracted from the restrictions, per relation.
CREATE FUNCTION pg_temp.ex8_advise(q text)
RETURNS TABLE (atoms text, candidate text, fate text, in_plan bool, benefit numeric)
LANGUAGE sql AS $$
  SELECT j->'advisor'->>'atoms', h->>'ddl', h->>'fate', (h->>'in_plan')::bool,
         (j->'advisor'->>'query_level_benefit')::numeric
  FROM (SELECT pathprobe_propose_json(q, '{"advise": true}')::jsonb AS j) s
  LEFT JOIN LATERAL jsonb_array_elements(j->'hypo_indexes') h ON true
$$;

-- A. Join: only the restriction on ex8_customers produces an atom.  The
-- join clause o.customer_id = c.id is not modeled, so no candidate on
-- ex8_orders (customer_id) is proposed, and nothing measures it.
SELECT * FROM pg_temp.ex8_advise(
  'SELECT * FROM ex8_orders o JOIN ex8_customers c ON c.id = o.customer_id WHERE c.region = 3');

-- B. IS NOT NULL is skipped: only customer_id becomes an atom.
SELECT * FROM pg_temp.ex8_advise(
  'SELECT * FROM ex8_orders WHERE total IS NOT NULL AND customer_id = 9');

-- C. An OR is not split into branches.  The whole clause becomes one
-- boolean expression atom; the candidate built from it loses.
SELECT * FROM pg_temp.ex8_advise(
  'SELECT * FROM ex8_orders WHERE status = 3 OR total = 7');

-- D. An existing index already covers status: the advisor proposes only a
-- covering variant, which loses.  A benefit of 0 is evidence too.
SELECT * FROM pg_temp.ex8_advise('SELECT * FROM ex8_orders WHERE status = 3');

-- E. A hypothesis the advisor did not generate can be supplied by hand.
-- The join-column index survives at its own relation, but the final plan
-- is still a hash join: fate is about the index's relation, in_plan about
-- the plan.  The "advisor" block reports the benefit even though the
-- advisor did not run (enabled = false).
SELECT h->>'name' AS hypo, h->>'fate' AS fate, (h->>'in_plan')::bool AS in_plan,
       (j->'advisor'->>'enabled')::bool AS advisor_enabled,
       (j->'advisor'->>'query_level_benefit')::numeric AS benefit
FROM (SELECT pathprobe_propose_json(
        'SELECT * FROM ex8_orders o JOIN ex8_customers c ON c.id = o.customer_id WHERE c.region = 3',
        '{"hypo_indexes":[{"name":"orders_customer","relname":"ex8_orders","columns":["customer_id"]}]}')::jsonb AS j) s,
     jsonb_array_elements(j->'hypo_indexes') h;
EXPLAIN (COSTS OFF)
SELECT * FROM ex8_orders o JOIN ex8_customers c ON c.id = o.customer_id WHERE c.region = 3;

-- Structural checks.
WITH a AS (SELECT * FROM pg_temp.ex8_advise(
  'SELECT * FROM ex8_orders o JOIN ex8_customers c ON c.id = o.customer_id WHERE c.region = 3')),
     b AS (SELECT * FROM pg_temp.ex8_advise(
  'SELECT * FROM ex8_orders WHERE total IS NOT NULL AND customer_id = 9')),
     c AS (SELECT * FROM pg_temp.ex8_advise(
  'SELECT * FROM ex8_orders WHERE status = 3 OR total = 7')),
     d AS (SELECT * FROM pg_temp.ex8_advise('SELECT * FROM ex8_orders WHERE status = 3')),
     e AS (SELECT h, j FROM (SELECT pathprobe_propose_json(
        'SELECT * FROM ex8_orders o JOIN ex8_customers c ON c.id = o.customer_id WHERE c.region = 3',
        '{"hypo_indexes":[{"name":"orders_customer","relname":"ex8_orders","columns":["customer_id"]}]}')::jsonb AS j) s,
     jsonb_array_elements(j->'hypo_indexes') h)
SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('A: no candidate on the join column of ex8_orders',
   (SELECT bool_and(candidate NOT LIKE '%ON ex8_orders%') FROM a)),
  ('A: the restriction on ex8_customers produced a candidate',
   (SELECT bool_or(candidate LIKE '%ON ex8_customers (region)%') FROM a)),
  ('B: IS NOT NULL produced no atom; customer_id did',
   (SELECT bool_and(atoms NOT LIKE '%total%' AND atoms LIKE '%customer_id=%') FROM b)),
  ('C: the OR clause became a single expression candidate, not branches',
   (SELECT count(*) = 1 AND bool_and(candidate LIKE '%OR%') FROM c)),
  ('C: and it did not help',
   (SELECT bool_and(NOT in_plan AND benefit = 0) FROM c)),
  ('D: with an existing index, no plain (status) candidate and no benefit',
   (SELECT bool_and(candidate LIKE '%INCLUDE%' AND benefit = 0) FROM d)),
  ('E: the hand-written join-column hypothesis survived at its relation',
   (SELECT bool_and(h->>'fate' = 'survived') FROM e)),
  ('E: but the final plan does not use it',
   (SELECT bool_and(NOT (h->>'in_plan')::bool) FROM e)),
  ('E: the advisor block is present although the advisor did not run',
   (SELECT bool_and(NOT (j->'advisor'->>'enabled')::bool
                    AND j->'advisor' ? 'query_level_benefit') FROM e)),
  ('E: EXPLAIN shows a hash join',
   pg_temp.ex_plan_node('SELECT * FROM ex8_orders o JOIN ex8_customers c ON c.id = o.customer_id WHERE c.region = 3') = 'Hash Join')) v(label, ok);

DROP TABLE ex8_orders, ex8_customers;
