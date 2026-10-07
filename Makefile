MODULE_big   = pathprobe
OBJS         = pathprobe.o pathprobe_probe1.o pathprobe_probe2.o pathprobe_json.o pathprobe_propose.o pathprobe_advisor.o pathprobe_gate.o pathprobe_security.o
EXTENSION    = pathprobe
DATA         = pathprobe--1.1.sql
REGRESS      = pathprobe pathprobe_propose_basic pathprobe_agent pathprobe_combo pathprobe_combo_generated pathprobe_security pathprobe_lifecycle pathprobe_graph pathprobe_kept pathprobe_hypo_cost pathprobe_advisor_bool
PG_CONFIG   ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_CPPFLAGS += -DHAVE_PATHPROBE_HOOKS
include $(PGXS)
