-- decided_by: the add_path() test that decided a REJECTED, DISPLACED or
-- REMOVED event.  pathprobe recomputes it from the two paths in the same
-- order as add_path(); "unknown" means the recomputation does not reach
-- add_path()'s decision.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
SET pathprobe.log_accepted = on;
CREATE TABLE db_c (id int PRIMARY KEY, region int);
CREATE TABLE db_o (id int PRIMARY KEY, customer_id int, amount int);
INSERT INTO db_c SELECT g, g % 10 FROM generate_series(1, 10000) g;
INSERT INTO db_o SELECT g, g % 10000 + 1, g % 97 FROM generate_series(1, 30000) g;
CREATE INDEX db_o_customer_idx ON db_o (customer_id);
ANALYZE db_c, db_o;

CREATE FUNCTION db_ev(j jsonb)
RETURNS TABLE (seq int, kind text, pathtype text, label text, path_id int,
               link_path_id int, link_seq int, gap_cost float8,
               disabled_nodes int, decided_by text)
LANGUAGE sql AS $$
  SELECT (e->>'seq')::int, e->>'kind', e->>'pathtype', e->>'label',
         (e->>'path_id')::int,
         coalesce(e->>'dominator_path_id', e->>'displacer_path_id',
                  e->>'remover_path_id')::int,
         coalesce(e->>'dominator_seq', e->>'displacer_seq',
                  e->>'remover_seq')::int,
         (e->>'gap_cost')::float8,
         coalesce((e->>'disabled_nodes')::int, 0), e->>'decided_by'
  FROM jsonb_array_elements(j->'events') e
$$;

-- each decision between proposed candidates, with the label of the other
-- path; all candidates are winner_forbidden, so they share disabled_nodes,
-- and have far fewer rows than the real scan, so it never dominates them
CREATE FUNCTION db_decisions(q text, cands jsonb)
RETURNS TABLE (label text, kind text, decided_by text, other text)
LANGUAGE sql AS $$
  WITH j AS (SELECT pathprobe_propose_json(q, jsonb_build_object('candidates', cands)::text)::jsonb AS j),
       ev AS (SELECT d.* FROM j, db_ev(j.j) d)
  SELECT e.label, e.kind, e.decided_by,
         (SELECT o.label FROM ev o WHERE o.path_id = e.link_path_id
            AND o.label IS NOT NULL LIMIT 1)
  FROM ev e
  WHERE e.kind IN ('rejected', 'displaced', 'removed') AND e.label IS NOT NULL
  ORDER BY e.seq
$$;

CREATE FUNCTION db_cand(name text, total float8, rows int,
                        planner jsonb DEFAULT '{}', startup float8 DEFAULT 0)
RETURNS jsonb LANGUAGE sql AS $$
  SELECT jsonb_build_object(
    'target', jsonb_build_object('relname', 'db_c'),
    'candidate', jsonb_build_object('pathname', name, 'provider', 't'),
    'cost', jsonb_build_object('startup_cost', startup, 'total_cost', total,
                               'rows', rows),
    'planner', planner,
    'mode', jsonb_build_object('winner_forbidden', true))
$$;

-- 1. cost: 10.5 is more than 1% above 10
SELECT * FROM db_decisions('SELECT * FROM db_c',
  jsonb_build_array(db_cand('cA', 10, 1), db_cand('cB', 10.5, 1)));

-- 2. costs within 1%, same pathkeys and outer rels: parallel safety, then
--    rows, then the near-exact cost comparison; all equal is a tie, and
--    add_path() keeps the old path
SELECT * FROM db_decisions('SELECT * FROM db_c',
  jsonb_build_array(db_cand('cA', 10, 1, '{"parallel_safe": false}'),
                    db_cand('cB', 10, 1)));
SELECT * FROM db_decisions('SELECT * FROM db_c',
  jsonb_build_array(db_cand('cA', 10, 2), db_cand('cB', 10, 1)));
SELECT * FROM db_decisions('SELECT * FROM db_c',
  jsonb_build_array(db_cand('cA', 10, 1), db_cand('cB', 9.99, 1)));
SELECT * FROM db_decisions('SELECT * FROM db_c',
  jsonb_build_array(db_cand('cA', 10, 1), db_cand('cB', 10, 1)));
-- with startup cost considered (LIMIT), the near-exact comparison finds
-- cB worse on total and better on startup: different, still a tie
SELECT * FROM db_decisions('SELECT * FROM db_c LIMIT 5',
  jsonb_build_array(db_cand('cA', 10, 1, '{}', 5.02),
                    db_cand('cB', 10.05, 1, '{}', 5)));

-- 3. pathkeys: cN removes cA (fuzzily as cheap, unordered) and is then
--    rejected by cB (fuzzily as cheap, ordered by more keys); both by
--    pathkeys.  cD loses to cB on cost.
SELECT * FROM db_decisions('SELECT * FROM db_c ORDER BY region, id',
  jsonb_build_array(db_cand('cA', 10, 1),
                    db_cand('cB', 10.15, 1, '{"pathkeys": ["region", "id"]}'),
                    db_cand('cN', 10.09, 1, '{"pathkeys": ["region"]}'),
                    db_cand('cD', 10.5, 1)));

-- 4. disabled_nodes decides before cost: a winner_forbidden candidate far
--    cheaper than the real seq scan, with its rows and parallel safety
--    (not parallel safe: parallel query is off), is rejected by it
SELECT d.kind, d.label, d.pathtype, dom.pathtype AS dominator,
       d.disabled_nodes, dom.disabled_nodes AS dominator_disabled_nodes,
       d.decided_by
FROM (SELECT pathprobe_propose_json('SELECT * FROM db_c',
        jsonb_build_object('candidates',
          jsonb_build_array(db_cand('cF', 1, 10000,
            '{"parallel_safe": false}')))::text)::jsonb AS j) j,
     db_ev(j.j) d JOIN db_ev(j.j) dom
       ON dom.seq = d.link_seq
WHERE d.kind = 'rejected' AND d.label = 'cF';

-- 5. with enable_seqscan = off, the disabled seq scan is displaced by a
--    more expensive index path: gap_cost is negative, decided_by says why
SET enable_seqscan = off;
SELECT kind, pathtype, disabled_nodes, gap_cost < 0 AS gap_negative,
       decided_by
FROM db_ev(pathprobe_json('SELECT * FROM db_o WHERE customer_id < 9000')::jsonb)
WHERE kind = 'displaced' AND pathtype = 'SeqScan';
RESET enable_seqscan;

-- 6. on planner-generated paths, every REJECTED event that names its
--    dominator and every DISPLACED and REMOVED event carries decided_by,
--    and the recomputation reaches add_path()'s decision ("unknown" never
--    appears), with joins and with each join method disabled
CREATE FUNCTION db_check(q text)
RETURNS TABLE (decisions bool, missing bigint, unknown bigint)
LANGUAGE sql AS $$
  WITH ev AS (SELECT * FROM db_ev(pathprobe_json(q)::jsonb)
              WHERE kind IN ('displaced', 'removed')
                 OR (kind = 'rejected' AND link_path_id IS NOT NULL))
  SELECT count(*) > 0, count(*) FILTER (WHERE decided_by IS NULL),
         count(*) FILTER (WHERE decided_by = 'unknown')
  FROM ev
$$;
SELECT * FROM db_check('SELECT * FROM db_c c JOIN db_o o ON o.customer_id = c.id WHERE c.id <= 50 ORDER BY c.id');
SELECT * FROM db_check('SELECT c.region, sum(o.amount) FROM db_c c JOIN db_o o ON o.customer_id = c.id GROUP BY c.region');
SET enable_nestloop = off;
SELECT * FROM db_check('SELECT * FROM db_c c JOIN db_o o ON o.customer_id = c.id WHERE c.region = 3');
RESET enable_nestloop;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT * FROM db_check('SELECT * FROM db_c c JOIN db_o o ON o.customer_id = c.id WHERE c.region = 3');
RESET enable_hashjoin;
RESET enable_mergejoin;

-- 7. events without an add_path() decision carry no decided_by
SELECT count(*) > 0 AS events,
       count(*) FILTER (WHERE decided_by IS NOT NULL) AS with_decided_by
FROM db_ev(pathprobe_json('SELECT * FROM db_c c JOIN db_o o ON o.customer_id = c.id WHERE c.id <= 50')::jsonb)
WHERE kind NOT IN ('rejected', 'displaced', 'removed');

DROP FUNCTION db_check(text);
DROP FUNCTION db_cand(text, float8, int, jsonb, float8);
DROP FUNCTION db_decisions(text, jsonb);
DROP FUNCTION db_ev(jsonb);
DROP TABLE db_c, db_o;
