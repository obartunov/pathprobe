-- combinatorial invariant test: generate all combinations of candidate
-- dimensions, run each through pathprobe_propose_json, assert invariants.
-- Output is counters only (stable under cost drift).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pathprobe;
RESET client_min_messages;

CREATE TABLE combo (id int, v int);
INSERT INTO combo SELECT g, g % 100 FROM generate_series(1, 2200) g;
ANALYZE combo;
CREATE INDEX combo_id ON combo (id);

DO $body$
DECLARE
  rel        text;   -- target exists / not
  forb       bool;   -- winner_forbidden
  cheap      bool;   -- cost side
  ordered    bool;   -- pathkeys claim
  hypoform   int;    -- 0 none, 1 json, 2 sql, 3 advise
  cap_small  bool;   -- tiny max_events
  spec       jsonb;
  j          jsonb;
  c          jsonb;
  h          jsonb;
  ncases     int := 0;
  nfail      int := 0;
  fate       text;
  msg        text;
BEGIN
  FOR rel, forb, cheap, ordered, hypoform, cap_small IN
    SELECT r, f, ch, o, hf, cs
    FROM unnest(ARRAY['combo','no_such_rel']) r,
         unnest(ARRAY[true,false]) f,
         unnest(ARRAY[true,false]) ch,
         unnest(ARRAY[true,false]) o,
         unnest(ARRAY[0,1,2,3]) hf,
         unnest(ARRAY[false,true]) cs
  LOOP
    ncases := ncases + 1;
    PERFORM set_config('pathprobe.max_events',
                       CASE WHEN cap_small THEN '16' ELSE '10000' END, true);
    spec := jsonb_build_object(
      'target', jsonb_build_object('relname', rel),
      'candidate', jsonb_build_object('pathname', 'cX', 'provider', 't'),
      'cost', jsonb_build_object(
         'total_cost', CASE WHEN cheap THEN 0.5 ELSE 99999.0 END,
         'rows', 10),
      'mode', jsonb_build_object('winner_forbidden', forb));
    IF ordered THEN
      spec := jsonb_set(spec, '{planner}',
                        jsonb_build_object('pathkeys', jsonb_build_array('id')));
    END IF;
    IF hypoform = 1 THEN
      spec := spec || jsonb_build_object('hypo_indexes',
        jsonb_build_array(jsonb_build_object(
          'relname', 'combo', 'name', 'hj', 'columns',
          jsonb_build_array('v'))));
    ELSIF hypoform = 2 THEN
      spec := spec || jsonb_build_object('hypo_sql',
        jsonb_build_array('CREATE INDEX hs ON combo (v) INCLUDE (id)'));
    ELSIF hypoform = 3 THEN
      spec := spec || jsonb_build_object('advise', true);
    END IF;

    BEGIN
      j := pathprobe_propose_json(
             'SELECT * FROM combo WHERE v > 90 ORDER BY id', spec::text)::jsonb;
    EXCEPTION WHEN OTHERS THEN
      nfail := nfail + 1;
      RAISE NOTICE 'case %: call failed: %', ncases, SQLERRM;
      CONTINUE;
    END;

    msg := NULL;
    -- I1: candidates[] present with exactly one entry
    IF jsonb_array_length(j->'candidates') <> 1 THEN
      msg := 'candidates length';
    END IF;
    c := j->'candidates'->0;
    fate := c->>'fate';
    -- I2: fate from the allowed alphabet
    IF msg IS NULL AND fate NOT IN
       ('target_not_found','not_examined','unknown','rejected',
        'unknown_after_accept','survived_at_point','survived',
        'displaced','survived_upper') THEN
      msg := 'fate alphabet: ' || fate;
    END IF;
    -- I3: injected iff target relation is in the query
    IF msg IS NULL AND (c->>'injected')::bool <> (rel = 'combo') THEN
      msg := 'injected vs target';
    END IF;
    -- I4: not injected => target not in the query (I3), so target_not_found
    IF msg IS NULL AND NOT (c->>'injected')::bool AND
       fate <> 'target_not_found' THEN
      msg := 'not-injected consistency';
    END IF;
    -- I4b: target_not_found / not_examined => no events of any kind
    IF msg IS NULL AND fate IN ('target_not_found', 'not_examined') AND
       ((c->'event_counts'->>'accepted')::int <> 0 OR
        (c->'event_counts'->>'rejected')::int <> 0 OR
        (c->'event_counts'->>'survived')::int <> 0 OR
        (c->'event_counts'->>'displaced')::int <> 0) THEN
      msg := 'not-examined consistency';
    END IF;
    -- I5: survived <= accepted, where acceptance is covered
    IF msg IS NULL AND (j->'coverage'->>'add_path_accepted')::bool AND
       (c->'event_counts'->>'survived')::int >
       (c->'event_counts'->>'accepted')::int THEN
      msg := 'survived > accepted';
    END IF;
    -- I7: displaced <= accepted (only an accepted path can be displaced)
    IF msg IS NULL AND (j->'coverage'->>'add_path_accepted')::bool AND
       (c->'event_counts'->>'displaced')::int >
       (c->'event_counts'->>'accepted')::int THEN
      msg := 'displaced > accepted';
    END IF;
    -- I6: hypo entries: in_plan -> injected; survived <= accepted
    FOR h IN SELECT * FROM jsonb_array_elements(coalesce(j->'hypo_indexes','[]'::jsonb))
    LOOP
      IF msg IS NULL AND (h->>'in_plan')::bool AND NOT (h->>'injected')::bool THEN
        msg := 'hypo in_plan without injected';
      END IF;
      IF msg IS NULL AND (h->>'survived')::int > (h->>'accepted')::int THEN
        msg := 'hypo survived > accepted';
      END IF;
      -- I6b: hypo fate: alphabet; inconsistent is public but never expected;
      -- never examined or generated => no events; not injected =>
      -- target_not_found / not_examined
      IF msg IS NULL AND coalesce(h->>'fate', '') NOT IN
         ('target_not_found','not_examined','not_generated','rejected',
          'displaced','survived_at_point','survived','unknown',
          'inconsistent') THEN
        msg := 'hypo fate alphabet: ' || coalesce(h->>'fate', '<null>');
      END IF;
      IF msg IS NULL AND h->>'fate' = 'inconsistent' THEN
        msg := 'hypo fate inconsistent';
      END IF;
      IF msg IS NULL AND h->>'fate' IN ('target_not_found','not_examined','not_generated') AND
         ((h->>'accepted')::int <> 0 OR (h->>'rejected')::int <> 0 OR
          (h->>'survived')::int <> 0 OR (h->>'displaced')::int <> 0) THEN
        msg := 'hypo not generated but has events';
      END IF;
      IF msg IS NULL AND NOT (h->>'injected')::bool AND
         h->>'fate' NOT IN ('target_not_found','not_examined') THEN
        msg := 'hypo not injected consistency';
      END IF;
    END LOOP;
    -- I7: advise => advisor block enabled with baseline/plan costs
    IF msg IS NULL AND hypoform = 3 AND
       NOT ((j->'advisor'->>'enabled')::bool
            AND j->'advisor' ? 'baseline_cost'
            AND j->'advisor' ? 'plan_cost') THEN
      msg := 'advisor block incomplete';
    END IF;
    -- I8: limits block present; truncated implies capped length
    IF msg IS NULL AND (j->'limits'->>'events_truncated')::bool AND
       jsonb_array_length(j->'events') > 16 AND cap_small THEN
      msg := 'truncated but over cap';
    END IF;
    -- I9: seq strictly monotone over emitted events
    IF msg IS NULL THEN
      PERFORM 1 FROM (
        SELECT (e->>'seq')::int s,
               lag((e->>'seq')::int) OVER () p
        FROM jsonb_array_elements(j->'events') e) q
      WHERE p IS NOT NULL AND s <= p;
      IF FOUND THEN
        msg := 'seq not monotone';
      END IF;
    END IF;

    IF msg IS NOT NULL THEN
      nfail := nfail + 1;
      RAISE NOTICE 'case % (rel=%, forb=%, cheap=%, ordered=%, hypo=%, cap=%): %',
        ncases, rel, forb, cheap, ordered, hypoform, cap_small, msg;
    END IF;
  END LOOP;
  RAISE NOTICE 'combinatorial: % cases, % failures', ncases, nfail;
  IF nfail > 0 THEN
    RAISE EXCEPTION 'combinatorial invariants failed';
  END IF;
END;
$body$;

DROP TABLE combo;
