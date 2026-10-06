-- survivor annotations in pathprobe JSON: "kept_reason" is a field of its
-- own; "label" (and origin "proposed") stays with proposed paths
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE kp_t (id int, v int);
INSERT INTO kp_t SELECT g, g % 100 FROM generate_series(1, 2000) g;
CREATE INDEX kp_t_id ON kp_t (id);
CREATE INDEX kp_t_v ON kp_t (v);
ANALYZE kp_t;
CREATE TABLE kp_p (id int, v int);
INSERT INTO kp_p SELECT g, g % 100 FROM generate_series(1, 2200) g;
ANALYZE kp_p;

-- 1. range + order keeps non-cheapest survivors (the pathprobe.sql case):
-- they are real paths with a kept_reason and no label
WITH j AS (SELECT pathprobe_json('SELECT * FROM kp_t WHERE v BETWEEN 5 AND 8 ORDER BY v')::jsonb AS j),
     e AS (SELECT e FROM j, jsonb_array_elements(j->'events') e)
SELECT e->>'src' AS src, e->>'pathtype' AS pathtype, e->>'kind' AS kind,
       e->>'kept_reason' AS kept_reason, e->>'origin' AS origin, e ? 'label' AS has_label
FROM e WHERE e ? 'kept_reason' ORDER BY (e->>'seq')::int;
WITH j AS (SELECT pathprobe_json('SELECT * FROM kp_t WHERE v BETWEEN 5 AND 8 ORDER BY v')::jsonb AS j),
     e AS (SELECT e FROM j, jsonb_array_elements(j->'events') e)
SELECT (SELECT bool_and(e->>'origin' = 'real') FROM e) AS all_real,
       (SELECT count(*) FROM e WHERE e ? 'label') AS labelled;

-- 2. the labelled events of a proposed candidate are still "proposed" and
-- carry no kept_reason
WITH j AS (SELECT pathprobe_propose_json(
             $$ SELECT * FROM kp_p WHERE id = 42 $$,
             '{"target":{"relname":"kp_p"},"candidate":{"pathname":"c_kept","provider":"t"},"cost":{"total_cost":0.5},"mode":{"winner_forbidden":true}}')::jsonb AS j),
     e AS (SELECT e FROM j, jsonb_array_elements(j->'events') e)
SELECT (SELECT count(*) > 0 FROM e WHERE e->>'label' = 'c_kept') AS has_label,
       (SELECT bool_and(e->>'origin' = 'proposed' AND NOT e ? 'kept_reason')
        FROM e WHERE e ? 'label') AS labelled_are_proposed;

-- 3. the text output still shows the annotation after the label column
SELECT line ~ 'survived  kept: (startup|order\?)$' AS text_kept
FROM regexp_split_to_table(pathprobe('SELECT * FROM kp_t WHERE v BETWEEN 5 AND 8 ORDER BY v'), E'\n') line
WHERE line LIKE '%kept:%';

DROP TABLE kp_t, kp_p;
