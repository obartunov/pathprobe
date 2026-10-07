-- advisor atoms from boolean quals: a boolean column or a boolean function
-- is an atom; an OR (any BoolExpr other than NOT over such an operand) and
-- other boolean expressions are not
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ab_t (id int PRIMARY KEY, a int, b int, c int, flag bool, ts date);
INSERT INTO ab_t SELECT g, g % 50, g % 70, g % 100, g % 10 = 0,
                        date '2026-01-01' + g % 365
FROM generate_series(1, 20000) g;
ANALYZE ab_t;

SELECT q.n, q.what,
       j->'advisor'->>'atoms' AS atoms,
       (SELECT string_agg(regexp_replace(h->>'ddl', '^CREATE INDEX \S+ ON ', ''), ' | '
                          ORDER BY h->>'ddl')
        FROM jsonb_array_elements(j->'hypo_indexes') h) AS candidates
FROM (VALUES
  (1, 'OR, two columns',     'SELECT * FROM ab_t WHERE c = 3 OR ts = ''2026-02-02'''),
  (2, 'OR, one column',      'SELECT * FROM ab_t WHERE ts = ''2026-02-02'' OR ts = ''2026-02-03'''),
  (3, 'AND inside OR',       'SELECT * FROM ab_t WHERE (a = 1 AND b = 2) OR c = 3'),
  (4, 'AND',                 'SELECT * FROM ab_t WHERE a = 1 AND b = 2'),
  (5, 'boolean column',      'SELECT * FROM ab_t WHERE flag'),
  (6, 'NOT boolean column',  'SELECT * FROM ab_t WHERE NOT flag'),
  (7, 'IS TRUE on a column', 'SELECT * FROM ab_t WHERE flag IS TRUE'),
  (8, 'boolean expression',  'SELECT * FROM ab_t WHERE (a > b) IS TRUE'),
  (9, 'boolean function',    'SELECT * FROM ab_t WHERE isfinite(ts)')
) q(n, what, sql),
LATERAL (SELECT pathprobe_propose_json(q.sql, '{"advise": true}')::jsonb AS j) p
ORDER BY q.n;

DROP TABLE ab_t;
