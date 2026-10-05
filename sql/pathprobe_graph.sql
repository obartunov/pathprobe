-- path identity and graph edges in pathprobe JSON
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
SET pathprobe.log_accepted = on;
CREATE TABLE gr_c (id int PRIMARY KEY, region int);
CREATE TABLE gr_o (id int PRIMARY KEY, customer_id int, amount int);
INSERT INTO gr_c SELECT g, g % 10 FROM generate_series(1, 10000) g;
INSERT INTO gr_o SELECT g, g % 10000 + 1, g % 97 FROM generate_series(1, 30000) g;
CREATE INDEX gr_o_customer_idx ON gr_o (customer_id);
ANALYZE gr_c, gr_o;

CREATE TEMP TABLE gr_j AS
SELECT pathprobe_json('SELECT * FROM gr_c c JOIN gr_o o ON o.customer_id = c.id WHERE c.id <= 5')::jsonb AS j;
CREATE TEMP VIEW gr_ev AS
SELECT (e->>'seq')::int AS seq, e->>'kind' AS kind, e->>'rel' AS rel,
       e->>'pathtype' AS pathtype, (e->>'path_id')::int AS path_id,
       (e->>'displacer_path_id')::int AS displacer_path_id,
       (e->>'displacer_seq')::int AS displacer_seq,
       e->>'other_pathtype' AS other_pathtype
FROM gr_j, jsonb_array_elements(j->'events') e;
CREATE TEMP VIEW gr_paths AS
SELECT (p->>'path_id')::int AS path_id, p->>'pathtype' AS pathtype,
       p->>'rel' AS rel, (p->>'subpath_id')::int AS subpath_id,
       (p->>'outer_path_id')::int AS outer_path_id,
       (p->>'inner_path_id')::int AS inner_path_id
FROM gr_j, jsonb_array_elements(j->'paths') p;

-- 1. every DISPLACED event names the displaced path (an earlier ACCEPTED
-- path_id) and the displacing path (the path_id of the ACCEPTED event at
-- displacer_seq), and carries the displacer's type
SELECT d.seq, d.pathtype, d.path_id = (SELECT a.path_id FROM gr_ev a
                                       WHERE a.kind = 'accepted' AND a.path_id = d.path_id
                                         AND a.seq < d.seq LIMIT 1) AS displaced_was_accepted,
       d.displacer_path_id = a2.path_id AS displacer_matches_seq,
       d.other_pathtype = a2.pathtype AS other_pathtype_matches
FROM gr_ev d JOIN gr_ev a2 ON a2.seq = d.displacer_seq AND a2.kind = 'accepted'
WHERE d.kind = 'displaced'
ORDER BY d.seq;

-- 2. the path accepted into the final rel is the nested loop of the plan;
-- its children are the scans EXPLAIN shows (the inner one through the
-- BitmapHeapScan -> IndexScan chain)
EXPLAIN (COSTS OFF) SELECT * FROM gr_c c JOIN gr_o o ON o.customer_id = c.id WHERE c.id <= 5;
WITH top AS (SELECT p.* FROM gr_ev e JOIN gr_paths p USING (path_id)
             WHERE e.rel = '?' AND e.kind = 'accepted')
SELECT top.pathtype AS top, o.pathtype AS outer_child, o.rel AS outer_rel,
       i.pathtype AS inner_child, i.rel AS inner_rel, s.pathtype AS inner_subpath
FROM top
JOIN gr_paths o ON o.path_id = top.outer_path_id
JOIN gr_paths i ON i.path_id = top.inner_path_id
LEFT JOIN gr_paths s ON s.path_id = i.subpath_id;

-- 3. a child that never had an event of its own still has a node: a
-- Memoize built inside a nested loop
SELECT p.pathtype, p.subpath_id IS NOT NULL AS has_subpath,
       NOT EXISTS (SELECT 1 FROM gr_ev e WHERE e.path_id = p.path_id) AS no_event
FROM gr_paths p WHERE p.pathtype = 'Memoize';

-- 4. structural invariants
SELECT
  (SELECT bool_and(path_id IS NULL) FROM gr_ev WHERE kind IN ('skipped', 'excluded'))
    AS no_path_no_id,
  (SELECT bool_and(path_id IS NOT NULL) FROM gr_ev WHERE kind NOT IN ('skipped', 'excluded'))
    AS every_path_event_has_id,
  (SELECT count(*) = max(path_id) AND count(DISTINCT path_id) = count(*) FROM gr_paths)
    AS ids_dense_and_unique,
  (SELECT bool_and(x IN (SELECT path_id FROM gr_paths))
   FROM (SELECT path_id AS x FROM gr_ev WHERE path_id IS NOT NULL
         UNION ALL SELECT displacer_path_id FROM gr_ev WHERE displacer_path_id IS NOT NULL
         UNION ALL SELECT subpath_id FROM gr_paths WHERE subpath_id IS NOT NULL
         UNION ALL SELECT outer_path_id FROM gr_paths WHERE outer_path_id IS NOT NULL
         UNION ALL SELECT inner_path_id FROM gr_paths WHERE inner_path_id IS NOT NULL) r)
    AS every_reference_resolves,
  (SELECT bool_and(EXISTS (SELECT 1 FROM gr_ev a WHERE a.kind = 'accepted'
                           AND a.path_id = s.path_id AND a.seq < s.seq))
   FROM gr_ev s WHERE s.kind = 'survived')
    AS survivors_keep_their_accepted_id,
  -- add_path() frees a rejected path; a later path at the same address must
  -- get a new id (this query reuses one: a rejected nested loop's memory
  -- holds the next one)
  (SELECT bool_and((SELECT count(*) FROM gr_ev o WHERE o.path_id = r.path_id) = 1)
   FROM gr_ev r WHERE r.kind = 'rejected' AND r.pathtype NOT LIKE 'Index%')
    AS rejected_ids_not_reused;

-- 5. without acceptance logging there are no DISPLACED events, but ids are
-- still assigned and still resolve
SET pathprobe.log_accepted = off;
WITH j AS (SELECT pathprobe_json('SELECT * FROM gr_c c JOIN gr_o o ON o.customer_id = c.id WHERE c.id <= 5')::jsonb AS j)
SELECT (SELECT count(*) FROM j, jsonb_array_elements(j->'events') e WHERE e->>'kind' = 'displaced') AS displaced,
       (SELECT bool_and((e->>'path_id')::int <= jsonb_array_length(j->'paths'))
        FROM j, jsonb_array_elements(j->'events') e WHERE e ? 'path_id' AND e->>'path_id' IS NOT NULL) AS ids_resolve
FROM j;
RESET pathprobe.log_accepted;

DROP TABLE gr_o, gr_c;
