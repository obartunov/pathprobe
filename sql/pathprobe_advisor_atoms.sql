-- advisor atom limit: atoms beyond the per-relation limit are not used for
-- candidates; they are counted in advisor.atoms_dropped and flagged by
-- advisor.atom_truncated.  advisor.truncated stays the candidate cap only.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE aa_t (id int PRIMARY KEY, a int, b int, c int, d int, e int,
                   f int, g int, flag bool, p text);
INSERT INTO aa_t SELECT i, i % 7, i % 11, i % 13, i % 17, i % 19, i % 23,
                        i % 29, i % 10 = 0,
                        CASE WHEN i % 100 = 0 THEN NULL ELSE 'x' END
FROM generate_series(1, 5000) i;
CREATE TABLE aa_u (id int PRIMARY KEY, w int);
INSERT INTO aa_u SELECT i, i % 31 FROM generate_series(1, 1000) i;
ANALYZE aa_t, aa_u;

CREATE FUNCTION aa_adv(q text, spec text DEFAULT '{"advise": true}')
RETURNS jsonb LANGUAGE sql AS $$
  SELECT pathprobe_propose_json(q, spec)::jsonb -> 'advisor'
$$;

-- the five fields: limit, seen = kept + dropped, flag = dropped > 0
SELECT q.n, q.what,
       (j->>'atom_limit')::int AS lim,
       (j->>'atoms_seen')::int AS seen,
       (j->>'atoms_kept')::int AS kept,
       (j->>'atoms_dropped')::int AS dropped,
       (j->>'atom_truncated')::bool AS atom_trunc,
       (j->>'truncated')::bool AS cand_trunc,
       j->>'atoms' AS atoms
FROM (VALUES
  (1, 'two atoms',
   'SELECT * FROM aa_t WHERE a = 1 AND b > 3'),
  (2, 'six atoms, at the limit',
   'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1'),
  (3, 'seven atoms',
   'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1 AND g = 1'),
  (4, 'eight restrictions, two dropped',
   'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1 AND flag AND p IS NULL'),
  (5, 'two quals on one dropped column',
   'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1 AND g > 2 AND g < 9'),
  (6, 'dropped expression atoms',
   'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1 AND (g + 1) = 3 AND (g + 1) > 0 AND (g + 2) = 4'),
  (7, 'join: limit is per relation',
   'SELECT * FROM aa_t t JOIN aa_u u ON u.id = t.id WHERE t.a = 1 AND t.b = 1 AND t.c = 1 AND t.d = 1 AND t.e = 1 AND t.f = 1 AND t.g = 1 AND u.w = 3')
) q(n, what, qry),
LATERAL aa_adv(q.qry) j
ORDER BY q.n;

-- the candidate cap and the atom limit are independent
SET pathprobe.max_advisor_candidates = 1;
SELECT q.n, q.what,
       (j->>'truncated')::bool AS cand_trunc,
       (j->>'atom_truncated')::bool AS atom_trunc,
       (j->>'atoms_dropped')::int AS dropped
FROM (VALUES
  (1, 'candidate cap only',
   'SELECT * FROM aa_t WHERE a = 1 AND b > 3'),
  (2, 'both',
   'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1 AND g = 1')
) q(n, what, qry),
LATERAL aa_adv(q.qry) j
ORDER BY q.n;
RESET pathprobe.max_advisor_candidates;

-- dropped atoms change no candidate: the eight-restriction query gets the
-- same candidates as its six kept atoms alone
SELECT (SELECT array_agg(regexp_replace(h->>'ddl', '^CREATE INDEX \S+ ', '')
                         ORDER BY h->>'name')
        FROM jsonb_array_elements(pathprobe_propose_json(
          'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1 AND flag AND p IS NULL',
          '{"advise": true}')::jsonb -> 'hypo_indexes') h)
     = (SELECT array_agg(regexp_replace(h->>'ddl', '^CREATE INDEX \S+ ', '')
                         ORDER BY h->>'name')
        FROM jsonb_array_elements(pathprobe_propose_json(
          'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1',
          '{"advise": true}')::jsonb -> 'hypo_indexes') h)
       AS same_candidates;

-- advisor off: no atoms, nothing dropped
SELECT j->>'enabled' AS enabled, j->>'atoms_seen' AS seen,
       j->>'atom_truncated' AS atom_trunc
FROM aa_adv('SELECT * FROM aa_t WHERE a = 1',
            '{"hypo_indexes": [{"name": "h_a", "relname": "aa_t", "columns": ["a"]}]}') j;

-- text output: the atoms line carries the per-relation count
SELECT l FROM regexp_split_to_table(pathprobe_propose(
  'SELECT * FROM aa_t WHERE a = 1 AND b = 1 AND c = 1 AND d = 1 AND e = 1 AND f = 1 AND flag AND p IS NULL',
  '{"advise": true}'), E'\n') l
WHERE l LIKE 'atoms%';

DROP FUNCTION aa_adv(text, text);
DROP TABLE aa_t, aa_u;
