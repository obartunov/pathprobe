-- candidate lifecycle: displacement, target identity, coverage
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE lc_a (id int PRIMARY KEY, v int);
CREATE TABLE lc_b (id int PRIMARY KEY, w text);
INSERT INTO lc_a SELECT g, g % 100 FROM generate_series(1, 2000) g;
INSERT INTO lc_b SELECT g, 'x' FROM generate_series(1, 2000) g;
ANALYZE lc_a, lc_b;

-- 1. a later, cheaper candidate displaces an earlier one; the earlier one
-- was in the pathlist right after its own add_path, but is displaced
SELECT c->>'pathname' AS pathname, c->>'fate' AS fate,
       (c->'event_counts'->>'displaced')::int AS displaced
FROM jsonb_array_elements(pathprobe_propose_json('SELECT * FROM lc_a WHERE id = 7',
  '{"candidates":[
    {"target":{"relname":"lc_a"},"candidate":{"pathname":"early","provider":"t"},"cost":{"total_cost":5.0,"rows":1},"planner":{"parallel_safe":false}},
    {"target":{"relname":"lc_a"},"candidate":{"pathname":"late","provider":"t"},"cost":{"total_cost":1.0,"rows":1},"planner":{"parallel_safe":false}}]}'
  )::jsonb->'candidates') c
ORDER BY 1;

-- 2. target identity: no such relation in the query, versus a relation in
-- the query that join removal drops before the injection point
SELECT tgt, j->'candidates'->0->>'fate' AS fate,
       j->'fate'->>'fate' = j->'candidates'->0->>'fate' AS same_as_toplevel
FROM (VALUES (1, 'no_such_rel', 'SELECT * FROM lc_a WHERE id = 7'),
             (2, 'lc_b', 'SELECT lc_a.* FROM lc_a LEFT JOIN lc_b ON lc_b.id = lc_a.id WHERE lc_a.id = 7'))
     q(n, tgt, sql),
LATERAL (SELECT pathprobe_propose_json(sql,
           format('{"target":{"relname":"%s"},"candidate":{"pathname":"c","provider":"t"},"cost":{"total_cost":0.5,"rows":1}}', tgt))::jsonb j) s
ORDER BY n;

-- 3. coverage: propose observes acceptance and displacement; with min_gap
-- set, rejections are not covered
SELECT pathprobe_propose_json('SELECT * FROM lc_a WHERE id = 7', '{"advise":true}')::jsonb->'coverage' AS coverage;
SET pathprobe.min_gap = 1;
SELECT pathprobe_json('SELECT * FROM lc_a WHERE id = 7')::jsonb->'coverage'->>'add_path_rejected' AS rejected_covered;
RESET pathprobe.min_gap;

-- 4. without an explicit candidate (hypo-only, advise-only) there is no
-- legacy top-level fate; with one, it is present
SELECT mode, j ? 'fate' AS has_toplevel_fate,
       jsonb_array_length(j->'candidates') AS ncandidates
FROM (VALUES (1, 'hypo', '{"hypo_indexes":[{"relname":"lc_a","columns":["v"]}]}'),
             (2, 'advise', '{"advise":true}'),
             (3, 'candidate', '{"target":{"relname":"lc_a"},"candidate":{"pathname":"c","provider":"t"},"cost":{"total_cost":0.5,"rows":1}}'))
     q(n, mode, spec),
LATERAL (SELECT pathprobe_propose_json('SELECT * FROM lc_a WHERE v = 3', spec)::jsonb j) s
ORDER BY n;

-- 5. hypothetical index fate.  One row per hypo: fate, reason, and the
-- build_index_paths() decisions seen for it
CREATE TABLE lc_h (id int PRIMARY KEY, a int, b int, pad text);
INSERT INTO lc_h SELECT g, g % 1000, g % 50, repeat('x', 40)
FROM generate_series(1, 20000) g;
ALTER TABLE lc_h SET (autovacuum_enabled = off);
VACUUM (ANALYZE) lc_h;
CREATE FUNCTION lc_hypo(q text, spec text)
RETURNS TABLE (name text, fate text, reason text, calls jsonb, generated int,
               not_generated jsonb, accepted int, survived int, displaced int)
LANGUAGE sql AS $$
  SELECT h->>'name', h->>'fate', h->>'fate_reason', h->'index_build'->'calls',
         (h->'index_build'->>'generated')::int, h->'index_build'->'not_generated',
         (h->>'accepted')::int, (h->>'survived')::int, (h->>'displaced')::int
  FROM jsonb_array_elements(pathprobe_propose_json(q, spec)::jsonb->'hypo_indexes') h
$$;

-- 5a. survived; not generated (no clause, ordering or index-only scan for
-- it); relation not in the query; relation in the query but removed
-- before planning
SELECT * FROM lc_hypo('SELECT * FROM lc_h WHERE a = 7',
  '{"hypo_indexes":[{"name":"on_a","relname":"lc_h","columns":["a"]},
                    {"name":"on_b","relname":"lc_h","columns":["b"]},
                    {"name":"other_rel","relname":"lc_b","columns":["w"]}]}');
SELECT * FROM lc_hypo('SELECT lc_h.* FROM lc_h LEFT JOIN lc_b ON lc_b.id = lc_h.id WHERE lc_h.a = 7',
  '{"hypo_indexes":[{"name":"removed","relname":"lc_b","columns":["w"]}]}');

-- 5b. partial index whose predicate is not implied: never reaches
-- build_index_paths(), so not examined rather than not generated
SELECT * FROM lc_hypo('SELECT * FROM lc_h WHERE a = 7',
  '{"hypo_sql":["CREATE INDEX partial_a ON lc_h (a) WHERE b = 5"]}');

-- 5c. call sites.  The OR arms build paths the plain call does not; they
-- end up only inside a BitmapOr, which carries no label, so the outcome is
-- unknown.  A hash index has no clause for the plain call (no native SAOP)
-- and builds a bitmap path in the non-native SAOP call.
SELECT * FROM lc_hypo('SELECT * FROM lc_h WHERE a = 7 OR b = 9',
  '{"hypo_indexes":[{"name":"or_a_b","relname":"lc_h","columns":["a","b"]}]}');
SELECT * FROM lc_hypo('SELECT * FROM lc_h WHERE a IN (1, 2, 3)',
  '{"hypo_sql":["CREATE INDEX hash_a ON lc_h USING hash (a)"]}');

-- 5d. a later hypo displaces every accepted path of an earlier one, although
-- the earlier one was in the pathlist right after its own injection (the
-- covering one wins by an index-only scan, hence the VACUUM above)
SELECT * FROM lc_hypo('SELECT a, b FROM lc_h WHERE a = 7',
  '{"hypo_indexes":[{"name":"narrow","relname":"lc_h","columns":["a"]},
                    {"name":"covering","relname":"lc_h","columns":["a","b"]}]}');

-- 5e. the text form carries the same fate
SELECT substring(line from 'fate=.*') AS fate
FROM regexp_split_to_table(pathprobe_propose('SELECT * FROM lc_h WHERE a = 7',
  '{"hypo_indexes":[{"name":"t_on_b","relname":"lc_h","columns":["b"]},
                    {"name":"t_other","relname":"lc_b","columns":["w"]}]}'), E'\n') line
WHERE line LIKE 'hypo:%';

-- 6. the text card's note follows the final candidate fate, not any
-- SURVIVED snapshot: only survived and survived_upper candidates "won"
-- (survived_at_point needs displacement or removal to be uncovered, which
-- the propose functions do not allow on a patched server)
CREATE TABLE lc_n (id int PRIMARY KEY, region int);
INSERT INTO lc_n SELECT g, g % 10 FROM generate_series(1, 10000) g;
CREATE TABLE lc_m (id int PRIMARY KEY, n_id int);
INSERT INTO lc_m SELECT g, g % 10000 + 1 FROM generate_series(1, 20000) g;
ANALYZE lc_n, lc_m;
CREATE FUNCTION lc_card(q text, spec text) RETURNS SETOF text
LANGUAGE sql AS $$
  SELECT line FROM regexp_split_to_table(pathprobe_propose(q, spec), E'\n') line
  WHERE line ~ '^(pathname|fate     |note     )'
$$;
-- 6a. cA survives its add_path(), is removed by cN, which cB then rejects
SELECT lc_card('SELECT * FROM lc_n ORDER BY region, id', '{"candidates": [
  {"target": {"relname": "lc_n"}, "candidate": {"pathname": "cA", "provider": "t"},
   "cost": {"total_cost": 10.0, "rows": 1}},
  {"target": {"relname": "lc_n"}, "candidate": {"pathname": "cB", "provider": "t"},
   "cost": {"total_cost": 10.15, "rows": 1}, "planner": {"pathkeys": ["region", "id"]}},
  {"target": {"relname": "lc_n"}, "candidate": {"pathname": "cN", "provider": "t"},
   "cost": {"total_cost": 10.09, "rows": 1}, "planner": {"pathkeys": ["region"]}},
  {"target": {"relname": "lc_n"}, "candidate": {"pathname": "cD", "provider": "t"},
   "cost": {"total_cost": 10.5, "rows": 1}}]}');
-- 6b. early survives its add_path() and is displaced by late
SELECT lc_card('SELECT * FROM lc_n WHERE id < 50', '{"candidates": [
  {"target": {"relname": "lc_n"}, "candidate": {"pathname": "early", "provider": "t"},
   "cost": {"total_cost": 5.0, "rows": 1}},
  {"target": {"relname": "lc_n"}, "candidate": {"pathname": "late", "provider": "t"},
   "cost": {"total_cost": 1.0, "rows": 1}}]}');
-- 6c. survived at the base rel, not carried up as itself
SELECT lc_card('SELECT * FROM lc_n n JOIN lc_m m ON m.n_id = n.id',
  '{"target": {"relname": "lc_m"}, "candidate": {"pathname": "j", "provider": "t"},
    "cost": {"total_cost": 100.0, "rows": 20000}}');

-- 6d. the snapshot's cheapest real path is labelled as what it is: taken
-- before injection, not the plan's winner
SELECT regexp_replace(line, '[0-9]+\.[0-9]+', 'N', 'g') AS line
FROM regexp_split_to_table(pathprobe_propose('SELECT * FROM lc_n n JOIN lc_m m ON m.n_id = n.id',
  '{"target": {"relname": "lc_m"}, "candidate": {"pathname": "j", "provider": "t"},
    "cost": {"total_cost": 100.0, "rows": 20000}}'), E'\n') line
WHERE line ~* 'winner|cheapest';

DROP FUNCTION lc_card(text, text);
DROP FUNCTION lc_hypo(text, text);
DROP TABLE lc_a, lc_b, lc_h, lc_n, lc_m;
