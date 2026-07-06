-- pathprobe--1.1.sql
-- Path-level planner observability for PostgreSQL master
--
-- Load via shared_preload_libraries = 'pathprobe' or: LOAD 'pathprobe';

-- Basic interface: probe0 only (no core patch required)
CREATE FUNCTION pathprobe(query text)
RETURNS text
AS 'pathprobe', 'pathprobe'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pathprobe(text) IS
'pathprobe: path survival events for a query.
probe0 (set_rel_pathlist_hook): survivors only.
REJECTED/ACCEPTED events require pathprobe-core-hooks.patch + probe1.
Output is chronological. seq is strictly monotone.';

CREATE FUNCTION pathprobe_json(query text)
RETURNS text
AS 'pathprobe', 'pathprobe_json'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pathprobe_json(text) IS
'pathprobe: path life events as JSON.
Same data as pathprobe(), machine-readable. events array + summary array.';
