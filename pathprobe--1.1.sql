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

CREATE FUNCTION pathprobe_propose(query text, spec text)
RETURNS text
AS 'pathprobe', 'pathprobe_propose'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pathprobe_propose(text, text) IS
'Inject a proposed path candidate into real planner competition.
spec is a JSON string describing the candidate (see docs).
Returns pathprobe lifecycle output showing the candidate fate.';

CREATE FUNCTION pathprobe_propose_json(query text, spec text)
RETURNS text
AS 'pathprobe', 'pathprobe_propose_json'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pathprobe_propose_json(text, text) IS
'JSON form of pathprobe_propose(): candidates[], advisor, hypo_indexes,
limits, per-event labels.  Machine-readable planner-aware gate input.';
