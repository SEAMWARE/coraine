#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
#
BINARY        = swBroker
CC            = gcc

# Include paths:
#   -I../          for external libs (kalloc/, kjson/, ktrace/, kargs/, kbase/, swRest/, swPlugin/, swJsonld/, swNgsild/)
#   -Isrc          for the broker's own lib/ headers (db/, plugin/, serviceRoutines/)
#   -Isrc/lib      for includes like "db/DbDriver.h" from service routines
INCLUDE       = -I.. -Isrc/lib -Isrc/app/swBroker

DFLAGS        =
CFLAGS        = -O2 -Wall -Wno-unused-function -fstack-protector-all $(DFLAGS) $(INCLUDE)

#
# Source files
#
APP_SOURCES   = src/app/swBroker/swBroker.c           \
                src/app/swBroker/ngsildServices.c

SR_SOURCES    = src/lib/serviceRoutines/getEntities.c  \
                src/lib/serviceRoutines/getEntity.c    \
                src/lib/serviceRoutines/postEntities.c

DB_SOURCES    = src/lib/db/dbInit.c                    \
                src/lib/db/dbClose.c                   \
                src/lib/db/tenant.c

PLUGIN_SOURCES = src/lib/plugin/pluginLoader.c

ALL_SOURCES   = $(APP_SOURCES) $(SR_SOURCES) $(DB_SOURCES) $(PLUGIN_SOURCES)
ALL_OBJS      = $(ALL_SOURCES:.c=.o)

#
# Static library paths (sw-libs and k-libs)
#
LIB_DIR       = ..
SW_LIBS       = $(LIB_DIR)/swRest/libswRest.a         \
                $(LIB_DIR)/swNgsild/libswNgsild.a      \
                $(LIB_DIR)/swJsonld/libswJsonld.a      \
                $(LIB_DIR)/swPlugin/libswPlugin.a

K_LIBS        = $(LIB_DIR)/kargs/libkargs.a            \
                $(LIB_DIR)/ktrace/libktrace.a           \
                $(LIB_DIR)/kprom/libkprom.a             \
                $(LIB_DIR)/khash/libkhash.a             \
                $(LIB_DIR)/klog/libklog.a               \
                $(LIB_DIR)/kalloc/libkalloc.a           \
                $(LIB_DIR)/kjson/libkjson.a             \
                $(LIB_DIR)/kbase/libkbase.a

#
# System libraries
#
SYS_LIBS      = -lmicrohttpd -lssl -lcrypto -lpthread -ldl -lm

#
# Plugin directories
#
PLUGIN_DIR     = plugins
RAMDB_DIR      = src/plugins/currentState/swRamDB
ADMIN_DIR      = src/plugins/api/admin

RAMDB_SOURCES  = $(RAMDB_DIR)/ramdbRegister.c $(RAMDB_DIR)/ramdbInit.c $(RAMDB_DIR)/ramdbClose.c \
                 $(RAMDB_DIR)/ramdbGlobals.c $(RAMDB_DIR)/ramdbEntityCreate.c \
                 $(RAMDB_DIR)/ramdbEntityRetrieve.c $(RAMDB_DIR)/ramdbEntityQuery.c \
                 $(RAMDB_DIR)/ramdbStore.c $(RAMDB_DIR)/ramdbGeoMatch.c
RAMDB_OBJS     = $(RAMDB_SOURCES:.c=.o)

ADMIN_SOURCES  = $(ADMIN_DIR)/adminRegister.c $(ADMIN_DIR)/adminHealth.c \
                 $(ADMIN_DIR)/adminVersion.c $(ADMIN_DIR)/adminLog.c \
                 $(ADMIN_DIR)/adminTenants.c $(ADMIN_DIR)/adminPlugins.c
ADMIN_OBJS     = $(ADMIN_SOURCES:.c=.o)

MONGOC_DIR     = src/plugins/currentState/mongoc
MONGOC_SOURCES = $(MONGOC_DIR)/mongocGlobals.c $(MONGOC_DIR)/mongocRegister.c \
                 $(MONGOC_DIR)/mongocInit.c $(MONGOC_DIR)/mongocClose.c \
                 $(MONGOC_DIR)/mongocEntityCreate.c $(MONGOC_DIR)/mongocEntityRetrieve.c \
                 $(MONGOC_DIR)/mongocEntityQuery.c $(MONGOC_DIR)/mongocGeoIndex.c \
                 $(MONGOC_DIR)/mongocTenantSetup.c $(MONGOC_DIR)/mongocVersion.c \
                 $(MONGOC_DIR)/mongocKjTreeToBson.c $(MONGOC_DIR)/mongocBsonToKjTree.c \
                 $(MONGOC_DIR)/mongocDotEscape.c
MONGOC_OBJS    = $(MONGOC_SOURCES:.c=.o)
MONGOC_CFLAGS  = $(shell pkg-config --cflags mongoc2)
MONGOC_LDFLAGS = $(shell pkg-config --libs mongoc2)

PLUGIN_BASE    ?= $(CFLAGS)
PLUGIN_CFLAGS  = $(PLUGIN_BASE) -fPIC -Isrc/plugins

#
# Targets
#
all: $(BINARY) $(PLUGIN_DIR)/swRamDB.so $(PLUGIN_DIR)/admin.so $(PLUGIN_DIR)/mongoc.so

LDFLAGS   ?=

$(BINARY): $(ALL_OBJS)
	$(CC) -rdynamic $(LDFLAGS) -o $@ $(ALL_OBJS) -Wl,--whole-archive $(SW_LIBS) $(K_LIBS) -Wl,--no-whole-archive $(SYS_LIBS)

$(PLUGIN_DIR)/swRamDB.so: $(RAMDB_OBJS)
	@mkdir -p $(PLUGIN_DIR)
	$(CC) -shared -o $@ $(RAMDB_OBJS) -lgeos_c -lm

$(PLUGIN_DIR)/admin.so: $(ADMIN_OBJS)
	@mkdir -p $(PLUGIN_DIR)
	$(CC) -shared -o $@ $(ADMIN_OBJS)

$(RAMDB_DIR)/%.o: $(RAMDB_DIR)/%.c
	$(CC) $(PLUGIN_CFLAGS) -c $< -o $@

$(ADMIN_DIR)/%.o: $(ADMIN_DIR)/%.c
	$(CC) $(PLUGIN_CFLAGS) -c $< -o $@

$(PLUGIN_DIR)/mongoc.so: $(MONGOC_OBJS)
	@mkdir -p $(PLUGIN_DIR)
	$(CC) -shared -o $@ $(MONGOC_OBJS) $(MONGOC_LDFLAGS)

$(MONGOC_DIR)/%.o: $(MONGOC_DIR)/%.c
	$(CC) $(PLUGIN_CFLAGS) $(MONGOC_CFLAGS) -DMONGOC_PLUGIN_VERSION=\"0.1.0\" -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

PREFIX          = /usr/local
INSTALL_PLUGIN  = /opt/seamware/plugins

install: all
	@mkdir -p $(PREFIX)/bin
	@mkdir -p $(INSTALL_PLUGIN)/db/currentState
	@mkdir -p $(INSTALL_PLUGIN)/api
	cat $(BINARY)                  > $(PREFIX)/bin/$(BINARY)         && chmod +x $(PREFIX)/bin/$(BINARY)
	cat $(PLUGIN_DIR)/mongoc.so    > $(INSTALL_PLUGIN)/db/currentState/mongoc.so
	cat $(PLUGIN_DIR)/swRamDB.so   > $(INSTALL_PLUGIN)/db/currentState/swRamDB.so
	cat $(PLUGIN_DIR)/admin.so     > $(INSTALL_PLUGIN)/api/admin.so

i:   install
d:   clean
	@$(MAKE) CFLAGS="-g -O0 -Wall -Wno-unused-function -fstack-protector-all $(DFLAGS) $(INCLUDE)"
di:  d install
ci:  clean all install

COV_DIR    = coverage
COV_CFLAGS = -g -O0 --coverage -Wall -Wno-unused-function -I.. -Isrc/lib -Isrc/app/swBroker

coverage:
	@$(MAKE) CFLAGS="$(COV_CFLAGS)" LDFLAGS="--coverage" PLUGIN_BASE="-O2 -Wall -Wno-unused-function -I.. -Isrc/lib -Isrc/app/swBroker"
	@find . -name '*.gcda' -delete
	@SW_BROKER=$(CURDIR)/$(BINARY) \
	SW_BROKER_EXTRA_PARAMS="--database $(CURDIR)/$(PLUGIN_DIR)/swRamDB.so --pretty-print 2 --foreground" \
	$(HOME)/git/swLibs/bin/swTest || true
	@mkdir -p $(COV_DIR)
	@gcovr --root $(CURDIR)/src --object-directory $(CURDIR) \
	      --html-details $(COV_DIR)/index.html --html-title "swBroker Coverage"
	@echo ""
	@echo "Coverage report: file://$(CURDIR)/$(COV_DIR)/index.html"

clean:
	rm -f $(ALL_OBJS) $(RAMDB_OBJS) $(ADMIN_OBJS) $(MONGOC_OBJS) $(BINARY)
	rm -rf $(PLUGIN_DIR) $(COV_DIR)
	find . -name '*.gcda' -name '*.gcno' -delete 2>/dev/null; true

.PHONY: all clean install i ci coverage
