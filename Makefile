# contrib/optpaths/Makefile

EXTENSION = optpaths
EXTVERSION = 0.1
PGFILEDESC = "Additional paths"
MODULES = optpaths
OBJS = nodeSelfjoin.o optpaths.o path_walker.o $(WIN32RES)

DATA = optpaths--0.1.sql
#DATA_built = optpaths--0.1.sql

MODULE_big = optpaths
ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/optpaths
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

