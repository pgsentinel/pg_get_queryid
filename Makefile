MODULE_big = pg_get_queryid
OBJS = pg_get_queryid.o 

EXTENSION = pg_get_queryid
DATA = pg_get_queryid--1.0.sql
PGFILEDESC = "pg_get_queryid - get queryid function"

LDFLAGS_SL += $(filter -lm, $(LIBS))

NO_INSTALLCHECK = 1

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
