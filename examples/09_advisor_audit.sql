-- Example 9: what the local advisor generates for common query patterns.
-- The advisor is a local candidate generator; pathprobe measures what
-- happened to each generated candidate.  Neither says a candidate is good
-- for a workload.
--   psql -X -f examples/09_advisor_audit.sql
-- See docs/examples/09-advisor-audit.md and docs/technical/advisor-audit.md.

\ir _common.sql

DROP TABLE IF EXISTS ex9_t, ex9_u;
CREATE TABLE ex9_t (id int PRIMARY KEY, a int, b int, c int, d text,
                    flag bool, ts date, payload text);
INSERT INTO ex9_t
SELECT g, g % 1000, g % 50, g % 7, 'v' || (g % 3000), (g % 10 = 0),
       date '2026-01-01' + (g % 365),
       CASE WHEN g % 100 = 0 THEN NULL ELSE repeat('p', 20) END
FROM generate_series(1, 30000) g;
CREATE TABLE ex9_u (id int PRIMARY KEY, t_id int, w int);
INSERT INTO ex9_u SELECT g, g * 7 % 30000 + 1, g % 13 FROM generate_series(1, 3000) g;
CREATE INDEX ex9_t_b_a ON ex9_t (b, a);
CREATE INDEX ex9_t_lower_d ON ex9_t (lower(d));
ANALYZE ex9_t, ex9_u;

-- One row per advisor candidate: the atoms the advisor extracted, the
-- candidate DDL, whether build_index_paths() built paths for it, its fate
-- and whether the final plan uses it.  A pattern with no candidate gets one
-- row with an empty candidate.
CREATE FUNCTION pg_temp.ex9_advise(q text, spec text DEFAULT '{"advise": true}')
RETURNS TABLE (atoms text, candidate text, built int, fate text, in_plan bool)
LANGUAGE sql AS $$
  SELECT j->'advisor'->>'atoms',
         substring(h->>'ddl' from ' ON (.*)$'),
         (h->'index_build'->>'generated')::int,
         h->>'fate', (h->>'in_plan')::bool
  FROM (SELECT pathprobe_propose_json(q, spec)::jsonb AS j) s
  LEFT JOIN LATERAL jsonb_array_elements(j->'hypo_indexes') h ON true
$$;

CREATE TEMP TABLE ex9_patterns (n int, pattern text, q text);
INSERT INTO ex9_patterns VALUES
  (1,  'equality',                 'SELECT * FROM ex9_t WHERE c = 3'),
  (2,  'range',                    'SELECT * FROM ex9_t WHERE ts BETWEEN ''2026-03-01'' AND ''2026-03-03'''),
  (3,  'equality + range',         'SELECT * FROM ex9_t WHERE c = 3 AND ts > ''2026-12-25'''),
  (4,  'ORDER BY ... LIMIT',       'SELECT * FROM ex9_t ORDER BY ts LIMIT 10'),
  (5,  'ORDER BY ... DESC LIMIT',  'SELECT * FROM ex9_t ORDER BY ts DESC LIMIT 10'),
  (6,  'GROUP BY',                 'SELECT c, count(*) FROM ex9_t GROUP BY c'),
  (7,  'join, no constant on t',   'SELECT * FROM ex9_u u JOIN ex9_t t ON t.c = u.w WHERE u.id = 3'),
  (19, 'join, constant via EC',    'SELECT * FROM ex9_u u JOIN ex9_t t ON t.c = u.w WHERE u.w = 3'),
  (8,  'OR, two columns',          'SELECT * FROM ex9_t WHERE c = 3 OR ts = ''2026-02-02'''),
  (9,  'OR, one column',           'SELECT * FROM ex9_t WHERE ts = ''2026-02-02'' OR ts = ''2026-02-03'''),
  (10, 'IS NULL',                  'SELECT * FROM ex9_t WHERE payload IS NULL'),
  (11, 'IS NOT NULL',              'SELECT id FROM ex9_t WHERE payload IS NOT NULL AND c = 2'),
  (12, 'expression',               'SELECT * FROM ex9_t WHERE (a + b) = 77'),
  (13, 'expression, index exists', 'SELECT * FROM ex9_t WHERE lower(d) = ''v7'''),
  (14, 'boolean column',           'SELECT * FROM ex9_t WHERE flag AND c = 3'),
  (15, 'keys of (b, a) reversed',  'SELECT * FROM ex9_t WHERE a = 7 AND b = 7'),
  (16, 'prefix of (b, a)',         'SELECT * FROM ex9_t WHERE b = 7'),
  (17, 'LIKE prefix',              'SELECT * FROM ex9_t WHERE d LIKE ''v1%'''),
  (18, 'eight restrictions',       'SELECT * FROM ex9_t WHERE a = 1 AND b = 1 AND c = 1 AND d = ''x'' AND ts = ''2026-01-01'' AND flag AND payload IS NULL AND id = 5');

SELECT p.n, p.pattern, r.*
FROM ex9_patterns p, LATERAL pg_temp.ex9_advise(p.q) r
ORDER BY p.n, r.candidate;

-- Structural checks.
CREATE TEMP TABLE ex9_result AS
SELECT p.n, r.* FROM ex9_patterns p, LATERAL pg_temp.ex9_advise(p.q) r;

SELECT pg_temp.ex_check(label, ok) AS "check"
FROM (VALUES
  ('every generated candidate built index paths (none not_generated)',
   (SELECT bool_and(built > 0) FROM ex9_result WHERE candidate IS NOT NULL)),
  ('ORDER BY ASC gets an ordered candidate',
   (SELECT bool_or(candidate = 'ex9_t (ts)') FROM ex9_result WHERE n = 4)),
  ('ORDER BY DESC gets no candidate',
   (SELECT bool_and(candidate IS NULL) FROM ex9_result WHERE n = 5)),
  ('join column t.c gets no candidate from the join clause alone',
   (SELECT bool_and(candidate IS NULL OR candidate NOT LIKE 'ex9_t%') FROM ex9_result WHERE n = 7)),
  ('a constant reaching t.c through the equivalence class does give one',
   (SELECT bool_or(candidate = 'ex9_t (c)') FROM ex9_result WHERE n = 19)),
  ('an OR becomes one boolean expression candidate, not per-branch indexes',
   (SELECT bool_and(cnt = 1 AND all_or) FROM
      (SELECT n, count(*) AS cnt, bool_and(candidate LIKE '%OR%') AS all_or
       FROM ex9_result WHERE n IN (8, 9) GROUP BY n) x)),
  ('the one-column OR gets no plain (ts) candidate',
   (SELECT bool_and(candidate <> 'ex9_t (ts)') FROM ex9_result WHERE n = 9)),
  ('IS NULL is an atom, IS NOT NULL is not',
   (SELECT bool_and(atoms LIKE '%payload=%') FROM ex9_result WHERE n = 10)
   AND (SELECT bool_and(atoms NOT LIKE '%payload%') FROM ex9_result WHERE n = 11)),
  ('an expression index that already exists is proposed again',
   (SELECT bool_or(candidate = 'ex9_t ((lower(d)))') FROM ex9_result WHERE n = 13)),
  ('existing (b, a) does not suppress (a, b)',
   (SELECT bool_or(candidate = 'ex9_t (a, b)') FROM ex9_result WHERE n = 15)),
  ('a prefix of an existing index is proposed without advise_skip_prefix',
   (SELECT bool_or(candidate = 'ex9_t (b)') FROM ex9_result WHERE n = 16)),
  ('... and dropped with it',
   (SELECT bool_and(candidate IS DISTINCT FROM 'ex9_t (b)')
    FROM pg_temp.ex9_advise('SELECT * FROM ex9_t WHERE b = 7',
                            '{"advise": true, "advise_skip_prefix": true}'))),
  ('LIKE gets no atom',
   (SELECT bool_and(candidate IS NULL) FROM ex9_result WHERE n = 17)),
  ('only six atoms are kept out of eight restrictions',
   (SELECT bool_and(atoms NOT LIKE '%flag%' AND atoms NOT LIKE '%payload%') FROM ex9_result WHERE n = 18))) v(label, ok);

DROP TABLE ex9_t, ex9_u;
