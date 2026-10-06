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
       e->>'other_pathtype' AS other_pathtype,
       (e->>'dominator_path_id')::int AS dominator_path_id,
       (e->>'dominator_seq')::int AS dominator_seq,
       e->>'dominator_pathtype' AS dominator_pathtype
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
         UNION ALL SELECT dominator_path_id FROM gr_ev WHERE dominator_path_id IS NOT NULL
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
-- still assigned and still resolve; REJECTED events still name their
-- dominator, but there is no ACCEPTED event to point at
SET pathprobe.log_accepted = off;
WITH j AS (SELECT pathprobe_json('SELECT * FROM gr_c c JOIN gr_o o ON o.customer_id = c.id WHERE c.id <= 5')::jsonb AS j)
SELECT (SELECT count(*) FROM j, jsonb_array_elements(j->'events') e WHERE e->>'kind' = 'displaced') AS displaced,
       (SELECT bool_and((e->>'path_id')::int <= jsonb_array_length(j->'paths'))
        FROM j, jsonb_array_elements(j->'events') e WHERE e ? 'path_id' AND e->>'path_id' IS NOT NULL) AS ids_resolve,
       (SELECT count(*) > 0
               AND bool_and((e->>'dominator_path_id')::int <= jsonb_array_length(j->'paths')
                            AND e->'dominator_seq' = 'null')
        FROM j, jsonb_array_elements(j->'events') e WHERE e->>'kind' = 'rejected') AS dominators_without_seq
FROM j;
RESET pathprobe.log_accepted;

-- 6. every REJECTED event names the old path add_path() found to dominate
-- it: that path was accepted earlier into the same rel (dominator_seq) and
-- has the reported type
SELECT r.seq, r.pathtype, r.dominator_pathtype,
       a.kind = 'accepted' AND a.path_id = r.dominator_path_id
         AND a.rel = r.rel AND a.seq < r.seq AS dominator_seq_is_its_acceptance,
       p.pathtype = r.dominator_pathtype AS dominator_pathtype_matches,
       r.dominator_path_id <> r.path_id AS not_itself
FROM gr_ev r
LEFT JOIN gr_ev a ON a.seq = r.dominator_seq
LEFT JOIN gr_paths p ON p.path_id = r.dominator_path_id
WHERE r.kind = 'rejected'
ORDER BY r.seq;

-- 7. paths with a list of inputs name all of them in child_path_ids, in
-- the path's own order; every child id resolves.  Upper-rel paths have
-- only ACCEPTED events, so acceptance logging is on.
SET pathprobe.log_accepted = on;
CREATE TEMP TABLE gr_multi AS
SELECT q.name, pathprobe_json(q.sql)::jsonb AS j
FROM (VALUES
  ('append', 'SELECT id FROM gr_c UNION ALL SELECT id FROM gr_o ORDER BY id LIMIT 5'),
  ('bitmap_or', 'SELECT * FROM gr_o WHERE customer_id = 1 OR id = 2'),
  ('bitmap_and', 'SELECT * FROM gr_o WHERE customer_id < 50 AND id < 200'),
  ('setop', 'SELECT id FROM gr_c INTERSECT SELECT customer_id FROM gr_o'),
  ('recursive', 'WITH RECURSIVE r(n) AS (SELECT 1 UNION SELECT n + 1 FROM r WHERE n < 5) SELECT * FROM r')
) q(name, sql);
CREATE TEMP VIEW gr_multi_paths AS
SELECT m.name, (p->>'path_id')::int AS path_id, p->>'pathtype' AS pathtype,
       p->'child_path_ids' AS kids, jsonb_array_length(m.j->'paths') AS npaths, m.j
FROM gr_multi m, jsonb_array_elements(m.j->'paths') p;
SELECT name, pathtype, count(*) > 0 AS present,
       bool_and(jsonb_array_length(kids) = 2) AS two_children,
       bool_and((SELECT bool_and(c::int BETWEEN 1 AND npaths AND c::int <> path_id)
                 FROM jsonb_array_elements_text(kids) c)) AS children_resolve,
       string_agg(DISTINCT (SELECT string_agg(cp->>'pathtype', ',' ORDER BY o)
                            FROM jsonb_array_elements_text(kids) WITH ORDINALITY c(id, o)
                            JOIN jsonb_array_elements(j->'paths') cp
                              ON (cp->>'path_id') = c.id), ' | ') AS child_types
FROM gr_multi_paths
WHERE kids IS NOT NULL
GROUP BY name, pathtype
ORDER BY name, pathtype;

-- child_path_ids appears only on such paths; single-input wrappers and
-- joins keep subpath_id / outer_path_id / inner_path_id
SELECT count(*) AS misplaced_child_lists
FROM gr_multi_paths
WHERE (kids IS NOT NULL) <> (pathtype IN ('Append', 'MergeAppend', 'BitmapAnd',
                                          'BitmapOr', 'SetOp', 'RecursiveUnion'));

-- a set operation's inputs are subquery scans; their subpath is the
-- subquery's own path
SELECT s.pathtype, c.pathtype AS child, sub.pathtype AS child_subpath
FROM gr_multi_paths s
CROSS JOIN LATERAL jsonb_array_elements_text(s.kids) k(id)
JOIN LATERAL (SELECT cp FROM jsonb_array_elements(s.j->'paths') cp
              WHERE cp->>'path_id' = k.id) c1 ON true
CROSS JOIN LATERAL (SELECT c1.cp->>'pathtype' AS pathtype) c
LEFT JOIN LATERAL (SELECT sp->>'pathtype' AS pathtype FROM jsonb_array_elements(s.j->'paths') sp
                   WHERE sp->>'path_id' = c1.cp->>'subpath_id') sub ON true
WHERE s.name = 'setop' AND s.pathtype = 'SetOp'
GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;
RESET pathprobe.log_accepted;

-- 8. add_path() can remove old paths and then reject the new path: cN
-- (ordered by region) removes cA (unordered, fuzzily as cheap) and is then
-- rejected by cB (ordered by region, id).  cA is freed without an event;
-- its id must be dropped, so that cD, allocated next at cA's address,
-- gets a new id.
CREATE TEMP TABLE gr_rb AS
SELECT pathprobe_propose_json('SELECT * FROM gr_c ORDER BY region, id', '{"candidates": [
  {"target": {"relname": "gr_c"}, "candidate": {"pathname": "cA", "provider": "t"},
   "cost": {"total_cost": 10.0, "rows": 1}, "mode": {"winner_forbidden": true}},
  {"target": {"relname": "gr_c"}, "candidate": {"pathname": "cB", "provider": "t"},
   "cost": {"total_cost": 10.15, "rows": 1}, "planner": {"pathkeys": ["region", "id"]},
   "mode": {"winner_forbidden": true}},
  {"target": {"relname": "gr_c"}, "candidate": {"pathname": "cN", "provider": "t"},
   "cost": {"total_cost": 10.09, "rows": 1}, "planner": {"pathkeys": ["region"]},
   "mode": {"winner_forbidden": true}},
  {"target": {"relname": "gr_c"}, "candidate": {"pathname": "cD", "provider": "t"},
   "cost": {"total_cost": 10.5, "rows": 1}, "mode": {"winner_forbidden": true}}
]}')::jsonb AS j;
CREATE TEMP VIEW gr_rb_ev AS
SELECT (e->>'seq')::int AS seq, e->>'kind' AS kind, e->>'rel' AS rel,
       e->>'label' AS label, (e->>'path_id')::int AS path_id,
       (e->>'dominator_path_id')::int AS dominator_path_id
FROM gr_rb, jsonb_array_elements(j->'events') e;
SELECT seq, label, kind,
       (SELECT label FROM gr_rb_ev a WHERE a.path_id = r.dominator_path_id
          AND a.kind = 'accepted' LIMIT 1) AS dominator_label
FROM gr_rb_ev r WHERE kind = 'rejected' AND label IS NOT NULL ORDER BY seq;
SELECT
  -- cA left the pathlist with cN, but no event says so (cN never entered it)
  (SELECT count(*) FROM gr_rb_ev WHERE label = 'cA' AND kind = 'displaced')
    AS ca_displaced_events,
  (SELECT a.path_id <> d.path_id FROM gr_rb_ev a, gr_rb_ev d
   WHERE a.label = 'cA' AND a.kind = 'accepted' AND d.label = 'cD' AND d.kind = 'rejected')
    AS removed_before_reject_ids_not_reused,
  -- all events of one path_id are of one path
  (SELECT bool_and(n = 1) FROM (SELECT count(DISTINCT coalesce(label, '')) AS n
                                FROM gr_rb_ev WHERE path_id IS NOT NULL GROUP BY path_id) x)
    AS one_label_per_id,
  (SELECT bool_and(x.path_id BETWEEN 1 AND jsonb_array_length(j->'paths'))
   FROM gr_rb, (SELECT path_id FROM gr_rb_ev WHERE path_id IS NOT NULL
                UNION ALL SELECT dominator_path_id FROM gr_rb_ev
                WHERE dominator_path_id IS NOT NULL) x)
    AS every_reference_resolves;

DROP TABLE gr_o, gr_c;
