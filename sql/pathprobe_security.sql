-- ACL-aware access control for pathprobe propose/advisor.
-- Verifies the SECURITY INVOKER model: non-superusers may probe only queries
-- and relations they can already inspect; superusers keep full behaviour.
LOAD 'pathprobe';

-- fixtures
CREATE TABLE sec_public (id int, v int);
INSERT INTO sec_public SELECT g, g % 100 FROM generate_series(1, 2000) g;
CREATE TABLE sec_private (id int, secret text);
INSERT INTO sec_private SELECT g, 'x' FROM generate_series(1, 500) g;
CREATE TABLE sec_cols (id int, pub int, secret text);
INSERT INTO sec_cols SELECT g, g % 10, 'x' FROM generate_series(1, 500) g;
CREATE TABLE sec_rls (id int, owner name, v int);
INSERT INTO sec_rls SELECT g, 'sec_agent', g FROM generate_series(1, 200) g;
ALTER TABLE sec_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY sec_rls_p ON sec_rls USING (owner = current_user);
CREATE VIEW sec_view AS SELECT * FROM sec_private;
CREATE SCHEMA sec_hidden;
CREATE TABLE sec_hidden.h (id int);
INSERT INTO sec_hidden.h SELECT generate_series(1, 50);
ANALYZE sec_public, sec_private, sec_cols, sec_rls, sec_hidden.h;

DROP ROLE IF EXISTS sec_agent;
CREATE ROLE sec_agent LOGIN;
GRANT SELECT ON sec_public TO sec_agent;
GRANT SELECT (id, pub) ON sec_cols TO sec_agent;   -- column-level, not secret
GRANT SELECT ON sec_rls TO sec_agent;
GRANT SELECT ON sec_view TO sec_agent;             -- view only, not base
GRANT SELECT ON sec_hidden.h TO sec_agent;         -- table only, not schema USAGE

-- helper: run under sec_agent, report OK / DENIED / OTHER
CREATE FUNCTION sec_try(q text, spec text) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM pathprobe_propose_json(q, spec);
  RETURN 'OK';
EXCEPTION
  WHEN insufficient_privilege THEN RETURN 'DENIED';
  WHEN feature_not_supported THEN RETURN 'DENIED';
  WHEN OTHERS THEN RETURN 'OTHER:' || SQLERRM;
END $$;

SET ROLE sec_agent;

-- 1. SELECT on public -> OK
SELECT 'case1' AS c, sec_try('SELECT * FROM sec_public WHERE id=42', '{"advise":true}');
-- 2. no SELECT on private -> denied
SELECT 'case2' AS c, sec_try('SELECT * FROM sec_private', '{"advise":true}');
-- 3a. column-level: allowed columns -> OK
SELECT 'case3a' AS c, sec_try('SELECT id, pub FROM sec_cols WHERE pub>5', '{"advise":true}');
-- 3b. column-level: forbidden column -> denied
SELECT 'case3b' AS c, sec_try('SELECT secret FROM sec_cols', '{"advise":true}');
-- 5. hypo_sql as non-superuser -> denied
SELECT 'case5' AS c, sec_try('SELECT * FROM sec_public',
                             '{"hypo_sql":["CREATE INDEX zz ON sec_public(id)"]}');
-- 6. structured hypo on allowed relation -> OK
SELECT 'case6' AS c, sec_try('SELECT * FROM sec_public WHERE v>5',
                             '{"hypo_indexes":[{"relname":"sec_public","columns":["v"]}]}');
-- 7. table SELECT but missing schema USAGE -> denied
SELECT 'case7' AS c, sec_try('SELECT * FROM sec_hidden.h', '{"advise":true}');
-- 8. RLS table -> denied initially
SELECT 'case8' AS c, sec_try('SELECT * FROM sec_rls', '{"advise":true}');
-- 9. view over private base -> denied
SELECT 'case9' AS c, sec_try('SELECT * FROM sec_view', '{"advise":true}');
-- 11. multi-statement string -> denied (cannot slip through one stmt at a time)
SELECT 'case11' AS c, sec_try('SELECT * FROM sec_public; SELECT 1', '{"advise":true}');
-- 12. CTE over private base -> denied (walker descends into cteList)
SELECT 'case12' AS c, sec_try('WITH c AS (SELECT * FROM sec_private) SELECT * FROM c', '{"advise":true}');
-- 13. correlated subquery over private -> denied (walker descends into sublink)
SELECT 'case13' AS c, sec_try('SELECT * FROM sec_public WHERE id IN (SELECT id FROM sec_private)', '{"advise":true}');
-- 14. CTE over readable base -> OK
SELECT 'case14' AS c, sec_try('WITH c AS (SELECT * FROM sec_public WHERE v>5) SELECT * FROM c', '{"advise":true}');

RESET ROLE;

-- 4. superuser: hypo_sql allowed (current behaviour)
SELECT 'case4' AS c,
       CASE WHEN pathprobe_propose_json('SELECT * FROM sec_public',
              '{"hypo_sql":["CREATE INDEX ww ON sec_public(id)"]}') IS NOT NULL
            THEN 'OK' END;

-- 10. output must not name a private relation: a denied private probe
--     produces no output at all (error before render).  Confirm the agent
--     cannot obtain any pathprobe output mentioning sec_private.
SET ROLE sec_agent;
SELECT 'case10' AS c,
       CASE WHEN position('sec_private' in
              coalesce(sec_try('SELECT * FROM sec_private', '{"advise":true}'), ''))
            = 0 THEN 'no-leak' ELSE 'LEAK' END;
RESET ROLE;

DROP FUNCTION sec_try(text, text);
REVOKE ALL ON sec_public, sec_cols, sec_rls, sec_view FROM sec_agent;
REVOKE ALL ON sec_hidden.h FROM sec_agent;
DROP OWNED BY sec_agent;
DROP ROLE sec_agent;
DROP VIEW sec_view;
DROP TABLE sec_public, sec_private, sec_cols, sec_rls;
DROP SCHEMA sec_hidden CASCADE;
