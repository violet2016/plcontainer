#------------------------------------------------------------------------------
# 
# Copyright (c) 2016-Present Pivotal Software, Inc
#
#------------------------------------------------------------------------------
MODULE_big = plcontainer
# for extension
EXTENSION = plcontainer

# Directories
SRCDIR = ./src
MGMTDIR = ./management
PYCLIENTDIR = src/pyclient/bin
RCLIENTDIR = src/rclient/bin

# the DATA is including files for extension use
DATA = $(MGMTDIR)/sql/plcontainer--1.0.0.sql $(MGMTDIR)/sql/plcontainer--unpackaged--1.0.0.sql plcontainer.control

ifeq ($(PLC_PG),yes)
  $(shell cd $(MGMTDIR)/sql/ && ln -sf plcontainer_pg--1.0.0.sql plcontainer_install.sql.in )
  $(shell cd $(MGMTDIR)/sql/ && cp -f plcontainer_pg--1.0.0.sql plcontainer--1.0.0.sql )
else
  $(shell cd $(MGMTDIR)/sql/ && ln -sf plcontainer_gp--1.0.0.sql plcontainer_install.sql.in )
  $(shell cd $(MGMTDIR)/sql/ && cp -f plcontainer_gp--1.0.0.sql plcontainer--1.0.0.sql )
endif

# DATA_built will built sql from .in file
DATA_built = $(MGMTDIR)/sql/plcontainer_install.sql $(MGMTDIR)/sql/plcontainer_uninstall.sql

# Files to build
# TODO: clean docker out from plcontainer
FILES = src/function_cache.c src/plc_backend_api.c src/plcontainer.c src/sqlhandler.c \
        src/containers.c src/message_fns.c src/plc_configuration.c src/plc_docker_api.c \
        src/plc_typeio.c src/subtransaction_handler.c \
        src/common/base_network.c src/common/comm_channel.c src/common/comm_connectivity.c src/common/comm_messages.c src/common/comm_dummy_plc.c
OBJS = $(foreach FILE,$(FILES),$(subst .c,.o,$(FILE)))

PGXS := $(shell pg_config --pgxs)
include $(PGXS)

ifeq ($(PLC_PG),yes)
	override CFLAGS += -DPLC_PG
#	override CFLAGS += -DPLC_PG  -Wno-sign-compare
endif

# FIXME: We might need a configure script to handle below checks later.
# See https://github.com/greenplum-db/plcontainer/issues/322

# Plcontainer version
PLCONTAINER_VERSION = $(shell git describe)
ifeq ($(PLCONTAINER_VERSION),)
  $(error can not determine the plcontainer version)
else
  ver = \#define PLCONTAINER_VERSION \"$(PLCONTAINER_VERSION)\"
  $(shell if [ ! -f src/common/config.h ] || [ "$(ver)" != "`cat src/common/config.h`" ]; then echo "$(ver)" > src/common/config.h; fi)
endif

PLCONTAINERDIR = $(DESTDIR)$(datadir)/plcontainer
INCLUDE_DIR = $(SRCDIR)/include

override CFLAGS += -Werror -Wextra -Wall -Wno-sign-compare -I$(INCLUDE_DIR)

ifeq ($(ENABLE_COVERAGE),yes)
  override CFLAGS += -coverage
  override SHLIB_LINK += -lgcov --coverage
  override CLIENT_CFLAGS += -coverage -O0 -g
  override CLIENT_LDFLAGS += -lgcov --coverage
else
  override CLIENT_CFLAGS += -O3 -g
endif

all: all-lib
	@echo "Build PL/Container Done."

install: all installdirs install-lib install-extra install-clients

clean: clean-clients clean-coverage
distclean: distclean-config

installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(bindir)'
	$(MKDIR_P) '$(PLCONTAINERDIR)'

uninstall: uninstall-lib
	rm -f '$(DESTDIR)$(bindir)/plcontainer'
	rm -rf '$(PLCONTAINERDIR)'

.PHONY: install-extra
install-extra: installdirs
	$(INSTALL_PROGRAM) '$(MGMTDIR)/bin/plcontainer'                         '$(DESTDIR)$(bindir)/plcontainer'
	$(INSTALL_DATA)    '$(MGMTDIR)/config/plcontainer_configuration.xml'    '$(PLCONTAINERDIR)/'
	$(INSTALL_DATA)    '$(MGMTDIR)/sql/plcontainer_install.sql'             '$(PLCONTAINERDIR)/'
	$(INSTALL_DATA)    '$(MGMTDIR)/sql/plcontainer_uninstall.sql'           '$(PLCONTAINERDIR)/'

.PHONY: install-clients
install-clients:
	$(MKDIR_P) '$(DESTDIR)$(bindir)/plcontainer_clients'
	cp $(PYCLIENTDIR)/* $(DESTDIR)$(bindir)/plcontainer_clients/
	cp $(RCLIENTDIR)/*  $(DESTDIR)$(bindir)/plcontainer_clients/

.PHONY: installcheck
installcheck:
	$(MAKE) -C tests tests

.PHONY: build-clients
build-clients:
	CC='$(CC)' CFLAGS='$(CLIENT_CFLAGS)' LDFLAGS='$(CLIENT_LDFLAGS)' $(MAKE) -C $(SRCDIR)/pyclient all
	CC='$(CC)' CFLAGS='$(CLIENT_CFLAGS)' LDFLAGS='$(CLIENT_LDFLAGS)' $(MAKE) -C $(SRCDIR)/rclient all

.PHONY: clean-clients
clean-clients:
	$(MAKE) -C $(SRCDIR)/pyclient clean
	$(MAKE) -C $(SRCDIR)/rclient clean

.PHONY: clean-coverage
clean-coverage:
	rm -f `find . -name '*.gcda' -print`
	rm -f `find . -name '*.gcno' -print`
	rm -rf coverage.info coverage_result

.PHONY: distclean-config
distclean-config:
	rm -f src/common/config.h

.PHONY: coverage-report
coverage-report:
	lcov -c -o coverage.info -d .
	genhtml coverage.info -o coverage_result
