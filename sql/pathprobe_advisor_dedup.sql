-- advisor dedup of expression candidates: an existing btree index whose
-- leading key matches the expression covers it, as a column index covers a
-- column candidate (only key: always skipped; leading key of a wider
-- index: skipped with advise_skip_prefix); partial indexes do not count
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;
CREATE FUNCTION ad_make(t text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, a int, d text, ts date)', t);
  EXECUTE format('INSERT INTO %I SELECT g, g %% 50, ''v'' || (g %% 5000),
                  date ''2026-01-01'' + g %% 365 FROM generate_series(1, 20000) g', t);
END $$;
SELECT ad_make(t) FROM unnest(ARRAY['ad_none', 'ad_eq', 'ad_wide', 'ad_other',
                                    'ad_part', 'ad_bool']) t;
CREATE INDEX ad_eq_ld ON ad_eq (lower(d));
CREATE INDEX ad_wide_ld_a ON ad_wide (lower(d), a);
CREATE INDEX ad_other_ud ON ad_other (upper(d));
CREATE INDEX ad_part_ld ON ad_part (lower(d)) WHERE a > 0;
CREATE INDEX ad_bool_fin ON ad_bool (isfinite(ts));
ANALYZE ad_none, ad_eq, ad_wide, ad_other, ad_part, ad_bool;

SELECT q.n, q.what, coalesce(c.candidates, '-') AS expression_candidates
FROM (VALUES
  (1, 'no index',                      'ad_none',  'lower(d) = ''v7''', '{"advise": true}'),
  (2, 'index on lower(d)',             'ad_eq',    'lower(d) = ''v7''', '{"advise": true}'),
  (3, 'index on (lower(d), a)',        'ad_wide',  'lower(d) = ''v7''', '{"advise": true}'),
  (4, '... with advise_skip_prefix',   'ad_wide',  'lower(d) = ''v7''', '{"advise": true, "advise_skip_prefix": true}'),
  (5, 'index on upper(d)',             'ad_other', 'lower(d) = ''v7''', '{"advise": true}'),
  (6, 'partial index on lower(d)',     'ad_part',  'lower(d) = ''v7''', '{"advise": true}'),
  (7, 'no index, boolean function',    'ad_none',  'isfinite(ts)',      '{"advise": true}'),
  (8, 'index on isfinite(ts)',         'ad_bool',  'isfinite(ts)',      '{"advise": true}')
) q(n, what, rel, qual, spec),
LATERAL (SELECT string_agg(regexp_replace(h->>'ddl', '^CREATE INDEX \S+ ON ', ''), ' | ')
                AS candidates
         FROM jsonb_array_elements(pathprobe_propose_json(
                format('SELECT * FROM %I WHERE %s', q.rel, q.qual), q.spec)::jsonb
                -> 'hypo_indexes') h
         WHERE h->>'ddl' LIKE '%((%') c
ORDER BY q.n;

DROP TABLE ad_none, ad_eq, ad_wide, ad_other, ad_part, ad_bool;
DROP FUNCTION ad_make(text);
