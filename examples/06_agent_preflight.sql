-- Example 6: pathprobe as evidence for a preflight decision.
-- pathprobe provides structured planner evidence; the policy is outside it.
--   psql -X -f examples/06_agent_preflight.sql
-- See docs/examples/06-agent-preflight.md.

\ir _common.sql

SET pathprobe.log_accepted = on;

DROP TABLE IF EXISTS ex6_events;
CREATE TABLE ex6_events (id int PRIMARY KEY, account int, kind int, payload text);
INSERT INTO ex6_events SELECT g, g % 2000, g % 7, repeat('p', 60) FROM generate_series(1, 50000) g;
ANALYZE ex6_events;

-- Evidence about the query as it would be planned now.  Every field comes
-- from pathprobe_json(); nothing here is a decision.
CREATE FUNCTION pg_temp.ex6_evidence(q text) RETURNS jsonb
LANGUAGE sql AS $$
WITH j AS (SELECT pathprobe_json(q)::jsonb AS j)
SELECT jsonb_build_object(
  'coverage_gaps', (SELECT coalesce(jsonb_agg(k ORDER BY k), '[]')
                    FROM jsonb_each(j->'coverage') c(k, v) WHERE v = 'false'),
  'truncated', (j->'limits'->>'events_truncated')::bool
               OR (j->'limits'->>'summary_truncated')::bool,
  -- base relations where only a sequential scan survived
  'seqscan_only', (SELECT coalesce(jsonb_agg(DISTINCT j->'rels'->>trim(both '{}' from s->>'rel')), '[]')
                   FROM jsonb_array_elements(j->'summary') s
                   WHERE s->>'strategy' = 'SeqScan' AND (s->>'survived')::int > 0
                     AND s->>'rel' NOT LIKE '%,%'
                     AND NOT EXISTS (SELECT 1 FROM jsonb_array_elements(j->'summary') s2
                                     WHERE s2->>'rel' = s->>'rel' AND s2->>'strategy' <> 'SeqScan'
                                       AND (s2->>'survived')::int > 0)),
  'excluded', (SELECT coalesce(jsonb_agg(DISTINCT e->>'pathtype'), '[]')
               FROM jsonb_array_elements(j->'events') e WHERE e->>'kind' = 'excluded'),
  'skipped', (SELECT count(*) FROM jsonb_array_elements(j->'events') e
              WHERE e->>'kind' = 'skipped'),
  'displaced', (SELECT count(*) FROM jsonb_array_elements(j->'events') e
                WHERE e->>'kind' = 'displaced'))
FROM j
$$;

-- Evidence about a remedy: what would happen to candidate indexes.
CREATE FUNCTION pg_temp.ex6_hypo(q text, spec text) RETURNS jsonb
LANGUAGE sql AS $$
SELECT jsonb_build_object(
  'index_build_covered', (j->'coverage'->>'index_build')::bool,
  'hypo', (SELECT jsonb_agg(jsonb_build_object('name', h->>'name', 'fate', h->>'fate',
                                               'in_plan', (h->>'in_plan')::bool)
                            ORDER BY h->>'name')
           FROM jsonb_array_elements(j->'hypo_indexes') h))
FROM (SELECT pathprobe_propose_json(q, spec)::jsonb AS j) s
$$;

\set q 'SELECT * FROM ex6_events WHERE account = 42'

-- 1. The query as it stands.  index_build is a gap here: plain
-- pathprobe_json() does not observe index build decisions; only the
-- propose functions do, for hypothetical indexes.
SELECT pg_temp.ex6_evidence(:'q') AS evidence;

-- 2. Candidate remedies.  "typo" names a relation the query does not use.
SELECT pg_temp.ex6_hypo(:'q',
  '{"hypo_indexes":[{"name":"by_account","relname":"ex6_events","columns":["account"]},
                    {"name":"by_kind","relname":"ex6_events","columns":["kind"]},
                    {"name":"typo","relname":"ex6_event","columns":["account"]}]}') AS remedies;

-- 3. A session setting removes a join strategy.  EXPLAIN shows a plan
-- without merge joins and does not say they were excluded.
SET enable_mergejoin = off;
SELECT pg_temp.ex6_evidence(
  'SELECT * FROM ex6_events a JOIN ex6_events b ON b.id = a.account WHERE a.kind = 3') AS evidence;
RESET enable_mergejoin;

-- 4. An illustrative policy over that evidence.  It is not part of
-- pathprobe; a real gate would add its own thresholds and relation sizes.
WITH ev AS (SELECT pg_temp.ex6_evidence(:'q') AS e),
     hy AS (SELECT pg_temp.ex6_hypo(:'q',
  '{"hypo_indexes":[{"name":"by_account","relname":"ex6_events","columns":["account"]}]}') AS h)
SELECT CASE
         WHEN (e->>'truncated')::bool THEN 'defer: evidence truncated'
         WHEN jsonb_array_length(e->'seqscan_only') > 0
              AND EXISTS (SELECT 1 FROM jsonb_array_elements(h->'hypo') x
                          WHERE x->>'fate' = 'survived' AND (x->>'in_plan')::bool)
           THEN 'suggest index, then run'
         WHEN jsonb_array_length(e->'seqscan_only') > 0 THEN 'run with statement_timeout'
         ELSE 'run'
       END AS illustrative_decision
FROM ev, hy;

-- Structural checks.
SET enable_mergejoin = off;
WITH e1 AS (SELECT pg_temp.ex6_evidence(:'q') AS e),
     h1 AS (SELECT pg_temp.ex6_hypo(:'q',
  '{"hypo_indexes":[{"name":"by_account","relname":"ex6_events","columns":["account"]},
                    {"name":"by_kind","relname":"ex6_events","columns":["kind"]},
                    {"name":"typo","relname":"ex6_event","columns":["account"]}]}') AS h),
     e2 AS (SELECT pg_temp.ex6_evidence(
  'SELECT * FROM ex6_events a JOIN ex6_events b ON b.id = a.account WHERE a.kind = 3') AS e)
SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('plain probe reports index_build as a coverage gap',
   (SELECT e->'coverage_gaps' = '["index_build"]'::jsonb FROM e1)),
  ('evidence is not truncated', (SELECT NOT (e->>'truncated')::bool FROM e1)),
  ('only a sequential scan survives for ex6_events',
   (SELECT e->'seqscan_only' = '["ex6_events"]'::jsonb FROM e1)),
  ('index on account: survived and in the plan',
   (SELECT bool_and(x->>'fate' = 'survived' AND (x->>'in_plan')::bool)
    FROM h1, jsonb_array_elements(h->'hypo') x WHERE x->>'name' = 'by_account')),
  ('index on kind: not generated',
   (SELECT bool_and(x->>'fate' = 'not_generated')
    FROM h1, jsonb_array_elements(h->'hypo') x WHERE x->>'name' = 'by_kind')),
  ('misspelled relation: target_not_found',
   (SELECT bool_and(x->>'fate' = 'target_not_found')
    FROM h1, jsonb_array_elements(h->'hypo') x WHERE x->>'name' = 'typo')),
  ('disabled merge join is visible as excluded',
   (SELECT e->'excluded' ? 'MergeJoin' FROM e2))) v(label, ok);
RESET enable_mergejoin;

DROP TABLE ex6_events;
