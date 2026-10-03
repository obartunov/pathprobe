-- Example 5: injected=true is not enough.  What happened to the
-- hypothetical index in the planner lifecycle?
--   psql -X -f examples/05_hypothetical_index_lifecycle.sql
-- See docs/examples/05-hypothetical-index-lifecycle.md.

\ir _common.sql

DROP TABLE IF EXISTS ex5_h;
CREATE TABLE ex5_h (id int PRIMARY KEY, a int, b int, pad text)
  WITH (autovacuum_enabled = off);
INSERT INTO ex5_h SELECT g, g % 1000, g % 50, repeat('x', 40) FROM generate_series(1, 20000) g;
-- VACUUM sets the visibility map, which makes index-only scan costs (used
-- by the displacement case) independent of autovacuum timing.
VACUUM (ANALYZE) ex5_h;

CREATE FUNCTION pg_temp.ex5_hypo(q text, spec text)
RETURNS TABLE (hypo text, injected bool, fate text, reason text,
               calls jsonb, not_generated jsonb, acc int, rej int, surv int,
               disp int, in_plan bool)
LANGUAGE sql AS $$
  SELECT h->>'name', (h->>'injected')::bool, h->>'fate', h->>'fate_reason',
         h->'index_build'->'calls', h->'index_build'->'not_generated',
         (h->>'accepted')::int, (h->>'rejected')::int, (h->>'survived')::int,
         (h->>'displaced')::int, (h->>'in_plan')::bool
  FROM jsonb_array_elements(pathprobe_propose_json(q, spec)::jsonb->'hypo_indexes') h
$$;

-- A. One query, four hypothetical indexes, four different fates.  All four
-- are injected=true.
SELECT * FROM pg_temp.ex5_hypo('SELECT * FROM ex5_h WHERE a = 7',
  '{"hypo_indexes":[{"name":"on_a","relname":"ex5_h","columns":["a"]},
                    {"name":"on_b","relname":"ex5_h","columns":["b"]}],
    "hypo_sql":["CREATE INDEX hash_b ON ex5_h USING hash (b)",
                "CREATE INDEX part_a ON ex5_h (a) WHERE b = 5"]}');

-- B. Generated, then rejected: a b-only filter on (a, b).
SELECT * FROM pg_temp.ex5_hypo('SELECT * FROM ex5_h WHERE b = 5',
  '{"hypo_indexes":[{"name":"on_a_b","relname":"ex5_h","columns":["a","b"]}]}');

-- C. Displaced: "narrow" is in the pathlist right after its injection
-- (surv = 1), then "covering" displaces every path of it (disp = acc).
SELECT * FROM pg_temp.ex5_hypo('SELECT a, b FROM ex5_h WHERE a = 7',
  '{"hypo_indexes":[{"name":"narrow","relname":"ex5_h","columns":["a"]},
                    {"name":"covering","relname":"ex5_h","columns":["a","b"]}]}');

-- D. Unknown: the OR arms build paths, which end up only inside a BitmapOr.
-- Its arms carry no hypo label, so the outcome is not observed.
SELECT * FROM pg_temp.ex5_hypo('SELECT * FROM ex5_h WHERE a = 7 OR b = 9',
  '{"hypo_indexes":[{"name":"or_a_b","relname":"ex5_h","columns":["a","b"]}]}');

-- Structural checks.
WITH a AS (SELECT * FROM pg_temp.ex5_hypo('SELECT * FROM ex5_h WHERE a = 7',
  '{"hypo_indexes":[{"name":"on_a","relname":"ex5_h","columns":["a"]},
                    {"name":"on_b","relname":"ex5_h","columns":["b"]}],
    "hypo_sql":["CREATE INDEX hash_b ON ex5_h USING hash (b)",
                "CREATE INDEX part_a ON ex5_h (a) WHERE b = 5"]}')),
     b AS (SELECT * FROM pg_temp.ex5_hypo('SELECT * FROM ex5_h WHERE b = 5',
  '{"hypo_indexes":[{"name":"on_a_b","relname":"ex5_h","columns":["a","b"]}]}')),
     c AS (SELECT * FROM pg_temp.ex5_hypo('SELECT a, b FROM ex5_h WHERE a = 7',
  '{"hypo_indexes":[{"name":"narrow","relname":"ex5_h","columns":["a"]},
                    {"name":"covering","relname":"ex5_h","columns":["a","b"]}]}')),
     d AS (SELECT * FROM pg_temp.ex5_hypo('SELECT * FROM ex5_h WHERE a = 7 OR b = 9',
  '{"hypo_indexes":[{"name":"or_a_b","relname":"ex5_h","columns":["a","b"]}]}'))
SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('A: all four injected', (SELECT bool_and(injected) FROM a)),
  ('A: on_a survived and is in the plan',
   (SELECT fate = 'survived' AND in_plan FROM a WHERE hypo = 'on_a')),
  ('A: on_b not generated (no_useful_input)',
   (SELECT fate = 'not_generated' AND not_generated ? 'no_useful_input' FROM a WHERE hypo = 'on_b')),
  ('A: hash_b not generated (no_leading_clause)',
   (SELECT fate = 'not_generated' AND not_generated ? 'no_leading_clause' FROM a WHERE hypo = 'hash_b')),
  ('A: part_a not examined (predicate_not_implied), zero build calls',
   (SELECT fate = 'not_examined' AND reason = 'predicate_not_implied'
           AND (calls->>'plain')::int + (calls->>'saop_bitmap')::int + (calls->>'or_arm')::int = 0
    FROM a WHERE hypo = 'part_a')),
  ('B: on_a_b generated, then rejected',
   (SELECT fate = 'rejected' AND rej > 0 AND acc = 0 FROM b)),
  ('C: narrow survived its injection but is displaced',
   (SELECT fate = 'displaced' AND surv > 0 AND disp = acc FROM c WHERE hypo = 'narrow')),
  ('C: covering survived',
   (SELECT fate = 'survived' FROM c WHERE hypo = 'covering')),
  ('D: or_a_b built paths in OR arms, outcome not observed',
   (SELECT fate = 'unknown' AND reason = 'outcome_not_observed'
           AND (calls->>'or_arm')::int > 0 FROM d))) v(label, ok);

DROP TABLE ex5_h;
