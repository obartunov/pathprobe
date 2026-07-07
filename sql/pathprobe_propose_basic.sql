SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE pb (id int, v int);
INSERT INTO pb SELECT g, g % 100 FROM generate_series(1, 2200) g;
ANALYZE pb;
-- proposed candidate injected, winner forbidden -> survives; label restored via provider
SELECT (j->'candidates'->0->>'fate') AS fate,
       (j->'candidates'->0->>'injected')::bool AS injected,
       EXISTS (SELECT 1 FROM jsonb_array_elements(j->'events') e
               WHERE e->>'label' = 'c_basic') AS label_restored
FROM (SELECT pathprobe_propose_json(
        $$ SELECT * FROM pb WHERE id = 42 $$,
        '{"target":{"relname":"pb"},"candidate":{"pathname":"c_basic","provider":"t"},"cost":{"total_cost":0.5},"mode":{"winner_forbidden":true}}')::jsonb AS j) s;
-- advisor on no-index predicate -> hypo in_plan
SELECT EXISTS (SELECT 1 FROM jsonb_array_elements(
         pathprobe_propose_json($$ SELECT * FROM pb WHERE id = 42 AND v > 90 $$,
           '{"advise": true}')::jsonb->'hypo_indexes') h
       WHERE (h->>'in_plan')::bool) AS advisor_found_in_plan;
DROP TABLE pb;
