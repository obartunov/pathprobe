MODULE_big   = pathprobe
OBJS         = pathprobe.o pathprobe_probe1.o pathprobe_probe2.o pathprobe_json.o pathprobe_propose.o
EXTENSION    = pathprobe
DATA         = pathprobe--1.1.sql
REGRESS      = pathprobe pathprobe_propose_basic
PG_CONFIG   ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_CPPFLAGS += -DHAVE_PATHPROBE_HOOKS
include $(PGXS)
