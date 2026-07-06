LOAD 'pathprobe';
CREATE EXTENSION IF NOT EXISTS pathprobe;
SET pathprobe.explain = on;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE pp_t(id int, v int);
INSERT INTO pp_t SELECT g, g % 100 FROM generate_series(1, 2000) g;
CREATE INDEX pp_t_id ON pp_t(id);
CREATE INDEX pp_t_v  ON pp_t(v);
ANALYZE pp_t;

-- point lookup: IndexScan wins; BitmapHeapScan rejected (probe1) -> all paths visible
EXPLAIN (COSTS OFF) SELECT * FROM pp_t WHERE id = 42;

-- range + order: reverse-case (kept: order? / kept: startup) on non-cheapest survivors
EXPLAIN (COSTS OFF) SELECT * FROM pp_t WHERE v BETWEEN 5 AND 8 ORDER BY v;

-- structured output: FORMAT JSON section must be valid machine-readable JSON
EXPLAIN (FORMAT JSON, COSTS OFF) SELECT * FROM pp_t WHERE id = 42;

-- negative: feature off -> no PathProbe section
SET pathprobe.explain = off;
EXPLAIN (COSTS OFF) SELECT * FROM pp_t WHERE id = 42;

DROP TABLE pp_t;
