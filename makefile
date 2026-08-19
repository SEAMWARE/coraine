#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
#
# Convenience wrapper around CMake.
#
PREFIX         = /usr/local
BUILD_RELEASE  = BUILD_RELEASE
BUILD_DEBUG    = BUILD_DEBUG
BUILD_COVERAGE = BUILD_COVERAGE
COV_DIR        = coverage
COV_REPORT     = $(COV_DIR)/index.html
COV_ETSI_DIR   = coverage-etsi
COV_LIBS       = corRest corNgsild corJsonld
CORTEST         = $(HOME)/git/corLibs/bin/corTest
# gcovr lives in the ETSI test-suite venv (gcov-15-aware). Override if needed.
GCOVR         ?= $(HOME)/git/ngsi-ld-test-suite/.venv/bin/gcovr

ifndef CPU_COUNT
	CPU_COUNT := $(shell nproc)
endif

PLUGIN_DIR = /opt/seamware/plugins
ETC_DIR    = /opt/seamware/etc

# contextSourceExtras (§ 5.2.40) — default opaque JSON config rendered on
# /info/sourceIdentity. Regenerated on every build with current version, git
# SHA and build timestamp; override at runtime via --contextSourceExtras.
CORAINE_VERSION   = 0.2.0
CORAINE_GIT_SHA   = $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
CORAINE_BUILD_AT  = $(shell date -u +%Y-%m-%dT%H:%M:%SZ)

all:        release

# Sibling-repo libs we link against. `make di` here recurses into each and
# runs their own `make di`, which is a no-op when nothing changed and a
# rebuild+install when something did. Without this, editing a lib's source
# leaves the broker linking against a stale .a (silent — link succeeds
# against last-installed copy).
SIBLING_LIBS = corRest corNgsild corJsonld
SIBLING_DIR  = $(HOME)/git

libs:
	@for lib in $(SIBLING_LIBS); do \
	  $(MAKE) -C $(SIBLING_DIR)/$$lib di || exit 1; \
	done

release: libs
	cmake -B $(BUILD_RELEASE) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_RELEASE) -j$(CPU_COUNT)

debug: libs
	cmake -B $(BUILD_DEBUG) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DEBUG) -j$(CPU_COUNT)

clean:
	rm -rf $(BUILD_RELEASE) $(BUILD_DEBUG) $(BUILD_COVERAGE) $(COV_DIR) $(COV_ETSI_DIR)

etc/contextSourceExtras.json: makefile
	@mkdir -p etc
	@printf '{\n  "version": "%s",\n  "gitSha": "%s",\n  "buildAt": "%s"\n}\n' \
	  "$(CORAINE_VERSION)" "$(CORAINE_GIT_SHA)" "$(CORAINE_BUILD_AT)" > $@

# install_from <build-dir> — copy broker + plugins + etc out of a build tree
define install_from
	mkdir -p $(PLUGIN_DIR)/db/currentState $(PLUGIN_DIR)/troe/temporal $(PLUGIN_DIR)/api $(ETC_DIR)
	cp -p $(1)/src/app/coraine/coraine                       $(PREFIX)/bin/
	cp -p $(1)/src/plugins/currentState/mongoc/mongoc.so       $(PLUGIN_DIR)/db/currentState/
	cp -p $(1)/src/plugins/currentState/corRamDB/corRamDB.so     $(PLUGIN_DIR)/db/currentState/
	cp -p $(1)/src/plugins/temporal/none/none.so               $(PLUGIN_DIR)/troe/temporal/
	cp -p $(1)/src/plugins/temporal/ramdb/ramdb.so             $(PLUGIN_DIR)/troe/temporal/
	cp -p $(1)/src/plugins/temporal/timescale/timescale.so     $(PLUGIN_DIR)/troe/temporal/
	cp -p $(1)/src/plugins/api/admin/admin.so                  $(PLUGIN_DIR)/api/
	cp -p etc/contextSourceExtras.json                         $(ETC_DIR)/
endef

install: etc/contextSourceExtras.json
	$(call install_from,$(BUILD_RELEASE))

install_debug: etc/contextSourceExtras.json
	$(call install_from,$(BUILD_DEBUG))

test:
	$(CORTEST)

coverage:
	cmake -B $(BUILD_COVERAGE) -DCMAKE_BUILD_TYPE=Coverage
	cmake --build $(BUILD_COVERAGE) -j$(CPU_COUNT)
	@find $(BUILD_COVERAGE) -name '*.gcda' -delete
	COR_BROKER=$(CURDIR)/$(BUILD_COVERAGE)/src/app/coraine/coraine \
	COR_BROKER_EXTRA_PARAMS="--database $(CURDIR)/$(BUILD_COVERAGE)/src/plugins/currentState/corRamDB/corRamDB.so --pretty-print 2 --foreground" \
	$(CORTEST) || true
	@mkdir -p $(COV_DIR)
	$(GCOVR) --root $(CURDIR)/src --object-directory $(BUILD_COVERAGE) \
	      --html-details $(COV_REPORT) --html-title "coraine Coverage"
	@echo ""
	@echo "Coverage report: file://$(CURDIR)/$(COV_REPORT)"

# Full ETSI-suite coverage: instrument the broker + NGSI-LD libs (corRest
# corNgsild corJsonld) + mongoc/timescale plugins, run the ETSI TP suite via
# etsiRun, then build an HTML report. The libs are static .a whole-archived
# into the broker, so they flush via the broker's gcov runtime (no per-.so
# coverage link needed); the plugin .so get --coverage from the Coverage build
# type (CMAKE_SHARED_LINKER_FLAGS).
coverage-etsi:
	@echo ">>> [1/6] Instrumenting NGSI-LD libs ($(COV_LIBS))..."
	@for d in $(COV_LIBS); do \
	   $(MAKE) -C ../$$d clean >/dev/null && \
	   $(MAKE) -C ../$$d DFLAGS="--coverage -O0 -Wno-error" lib$$d.a || exit 1; \
	 done
	@echo ">>> [2/6] Building broker + plugins (Coverage)..."
	cmake -B $(BUILD_COVERAGE) -DCMAKE_BUILD_TYPE=Coverage
	cmake --build $(BUILD_COVERAGE) -j$(CPU_COUNT)
	@echo ">>> [3/6] Installing instrumented broker + plugins (etsiRun loads the installed ones)..."
	$(MAKE) install_from_coverage
	@echo ">>> [4/6] Wiping stale .gcda counters across build + lib trees..."
	@find $(BUILD_COVERAGE) $(addprefix ../,$(COV_LIBS)) -name '*.gcda' -delete
	@echo ">>> [5/6] Running the ETSI TP suite (etsiRun sw)..."
	@etsiRun sw || true
	@echo ">>> Stopping broker so gcov flushes .gcda (onSignal -> exit(0))..."
	@if [ -f /tmp/etsi-coraine.pid ]; then \
	   P=$$(cat /tmp/etsi-coraine.pid); \
	   kill "$$P" 2>/dev/null || true; \
	   for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$$P" 2>/dev/null && sleep 1 || break; done; \
	 fi
	@echo ">>> [6/6] Generating coverage report..."
	@mkdir -p $(COV_ETSI_DIR)
	@$(GCOVR) --root $(HOME)/git --gcov-executable gcov \
	      --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
	      -f '$(CURDIR)/src/lib/' -f '$(CURDIR)/src/app/' \
	      -f '$(CURDIR)/src/plugins/currentState/mongoc/' \
	      -f '$(CURDIR)/src/plugins/temporal/timescale/' \
	      -f '$(CURDIR)/src/plugins/shared/' \
	      -f '$(HOME)/git/corRest/' -f '$(HOME)/git/corNgsild/' -f '$(HOME)/git/corJsonld/' \
	      --html-details $(COV_ETSI_DIR)/index.html --html-title "coraine ETSI Coverage" \
	      $(BUILD_COVERAGE) $(addprefix $(HOME)/git/,$(COV_LIBS))
	@echo ""
	@echo "ETSI coverage report: file://$(CURDIR)/$(COV_ETSI_DIR)/index.html"

install_from_coverage: etc/contextSourceExtras.json
	$(call install_from,$(BUILD_COVERAGE))

i:          release install
di:         debug install_debug
ci:         clean release install
cdi:        clean debug install_debug

.PHONY: all release debug clean install install_debug install_from_coverage test coverage coverage-etsi i di ci cdi libs
