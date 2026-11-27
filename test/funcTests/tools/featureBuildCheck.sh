#!/bin/bash
#
# FILE            featureBuildCheck.sh
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# Build the broker in several reduced configurations and check each one STARTS.
#
# The functional suite runs against one installed binary, so it can only ever
# test the configuration that binary was built with. Everything else - does
# REGISTRATIONS=OFF still link, does the plugin still dlopen, does the process
# get as far as answering a request - is invisible to it, and those are exactly
# the failures a reduced build produces. They are also the cheap ones to catch:
# a broker that does not start does not need a test suite to notice.
#
# Prints ONE line per configuration and nothing else on success, so the caller
# can compare it as expected output. Build logs go to files and only the tail of
# one is printed, and only when it failed.
#
# Not run by the ordinary suite: ten cmake builds is minutes, against two
# seconds for every other case. `corTest -buildTests yes` opts in.
#
WORK=${COR_BUILD_TEST_DIR:-/tmp/coraine-featureBuild}
PORT=${COR_BUILD_TEST_PORT:-1039}
SRC=$(pwd)

# Fixed path, not mktemp: the teardown section of a test runs as its own
# process and cannot be told a random name.
rm -rf "$WORK"
mkdir -p "$WORK"

#
# The configurations. Each is a label plus the cmake flags that define it.
#
# Only the flags that actually DO something are here. Eight of the
# features are declared and unread (see doc/building-details.md), so a case for one of
# them would build a byte-identical binary and assert nothing; and three do not
# link at all, which is a known state rather than something to pin.
#
CONFIGS=(
  "REGISTRATIONS=OFF|-DCOR_FEATURE_REGISTRATIONS=OFF"
  "SUBSCRIPTIONS=OFF|-DCOR_FEATURE_SUBSCRIPTIONS=OFF"
  "REGISTRATIONS+SUBSCRIPTIONS=OFF|-DCOR_FEATURE_REGISTRATIONS=OFF -DCOR_FEATURE_SUBSCRIPTIONS=OFF"
  "MONGOC=OFF|-DCOR_FEATURE_MONGOC=OFF"
  "CORDB=OFF|-DCOR_FEATURE_CORDB=OFF"
  "ADMIN_API=OFF|-DCOR_FEATURE_ADMIN_API=OFF"
  "METRICS=OFF|-DCOR_FEATURE_METRICS=OFF"
  "TRANSPORTS=OFF|-DCOR_FEATURE_TRANSPORTS=OFF"
  "HA_RUNS=OFF|-DCOR_FEATURE_HA_RUNS=OFF"
  "AUTH=OFF|-DCOR_FEATURE_AUTH=OFF"
  "POLICY_API=OFF|-DCOR_FEATURE_POLICY_API=OFF"
  "SERVICE_EXECUTION=OFF|-DCOR_FEATURE_SERVICE_EXECUTION=OFF"
  "SNAPSHOT_WRITE=OFF|-DCOR_FEATURE_SNAPSHOT_WRITE=OFF"
  "SNAPSHOT_SUBSCRIPTIONS=OFF|-DCOR_FEATURE_SNAPSHOT_SUBSCRIPTIONS=OFF"
  "MIGRATE=OFF|-DCOR_FEATURE_MIGRATE=OFF"
  "HEALTH=OFF|-DCOR_FEATURE_HEALTH=OFF"
  "RESPONSE_BUDGET=OFF|-DCOR_FEATURE_RESPONSE_BUDGET=OFF"
  "AUTO_ENTITY_MAP=OFF|-DCOR_FEATURE_AUTO_ENTITY_MAP=OFF"
  "CHANNEL_STATUS_IN=OFF|-DCOR_FEATURE_CHANNEL_STATUS_IN=OFF"
  "DATA_CHANNELS=OFF|-DCOR_FEATURE_DATA_CHANNELS=OFF"
  "GEOQ=OFF|-DCOR_FEATURE_GEOQ=OFF"
  "TIMEPOINT=OFF|-DCOR_FEATURE_TIMEPOINT=OFF"
  "TEMPORAL_ENTITY_MAP=OFF|-DCOR_FEATURE_TEMPORAL_ENTITY_MAP=OFF"
  "SSE=OFF|-DCOR_FEATURE_SSE=OFF"
  "Q_PREFILTER=OFF|-DCOR_FEATURE_Q_PREFILTER=OFF"
  "DB_INDEXES=OFF|-DCOR_FEATURE_DB_INDEXES=OFF"
  "PROBES=OFF|-DCOR_FEATURE_PROBES=OFF"
  "LOG_LINES=OFF|-DCOR_FEATURE_LOG_LINES=OFF"
  "LOG_TRIM=OFF|-DCOR_FEATURE_LOG_TRIM=OFF"
  "TROE_CORDB=OFF|-DCOR_FEATURE_TROE_CORDB=OFF"
  "TROE_TIMESCALE=OFF|-DCOR_FEATURE_TROE_TIMESCALE=OFF"
)

rc=0

# COR_BUILD_TEST_ONLY: a comma-separated list of labels (MONGOC=OFF,CORDB=OFF) - those alone, for working on one
for cfg in "${CONFIGS[@]}"; do
  label="${cfg%%|*}"
  flags="${cfg#*|}"
  [ -z "$COR_BUILD_TEST_ONLY" ] || [[ ",$COR_BUILD_TEST_ONLY," == *",$label,"* ]] || continue
  d="$WORK/$(echo "$label" | tr '=+' '__')"

  if ! cmake -S "$SRC" -B "$d" -DCMAKE_BUILD_TYPE=Debug $flags > "$d.cfg.log" 2>&1; then
    echo "$label: configure FAILED"
    tail -5 "$d.cfg.log"
    rc=1
    continue
  fi

  if ! cmake --build "$d" -j"$(nproc)" > "$d.bld.log" 2>&1; then
    echo "$label: build FAILED"
    grep -E "error:|undefined reference" "$d.bld.log" | head -5
    rc=1
    continue
  fi

  #
  # Started with corDB and no TRoE on purpose: this checks that the process
  # comes up, not that a database is reachable, and a configuration that needed
  # mongo to answer would be untestable in exactly the case (MONGOC=OFF) that
  # most needs testing.
  #
  # corDB is a repo of its own (../corDB): built for this configuration too - a plugin carries the
  # broker's feature switches - in a directory of its own, the tree's own builds untouched.
  #
  # GEOQ is corDB's COR_WITH_GEOQ, as the broker's makefile passes it. CORAINE: this tree's shared sources
  # (geoMatch.c, dbPluginAbi.c) - by default corDB compiles ../coraine's, another checkout's
  #
  # CORDB=OFF: the corDB repository is not built at all (the makefile's COR_WITH_CORDB=0)
  P="$d.plugins"
  mkdir -p "$P/db/currentState" "$P/troe/temporal" "$P/api"
  if [ "$label" != CORDB=OFF ]; then
  dbFlags=$(echo "$flags" | tr ' ' '\n' | sed -n 's/^-D\(COR_FEATURE_[A-Z_]*\)=OFF$/\1=0/p' | sed -e 's/^COR_FEATURE_GEOQ=0$/COR_WITH_GEOQ=0/' -e 's/^COR_FEATURE_TROE_CORDB=0$/COR_WITH_TROE_CORDB=0/' | tr '\n' ' ')
  if ! make -C "$SRC/../corDB" OBJDIR="$d.corDB" CORAINE="$SRC" $dbFlags > "$d.corDB.log" 2>&1; then
    echo "$label: corDB build FAILED"
    grep -E "error:|undefined reference" "$d.corDB.log" | head -5
    rc=1
    continue
  fi
  cp -p "$d.corDB/corDB.so"                          "$P/db/currentState/" 2>/dev/null
  fi
  cp -p "$d/src/plugins/currentState/mongoc/mongoc.so" "$P/db/currentState/" 2>/dev/null   # not built: MONGOC=OFF
  cp -p "$d/src/plugins/temporal/none/none.so"       "$P/troe/temporal/"   2>/dev/null
  cp -p "$d/src/plugins/temporal/timescale/timescale.so" "$P/troe/temporal/" 2>/dev/null   # not built: TROE_TIMESCALE=OFF

  #
  # MONGOC=OFF and CORDB=OFF start with no --database: the default is the one store built. CORDB=OFF's is
  # mongoc, which needs a MongoDB (every functest job has one; corTest's COR_MONGO_HOST/PORT)
  #
  case "$label" in
    MONGOC=OFF) dbArgs=() ;;
    CORDB=OFF)  dbArgs=(--dbName featureBuild --dbHost "${COR_MONGO_HOST:-localhost}" --dbPort "${COR_MONGO_PORT:-27017}") ;;
    *)          dbArgs=(--database corDB) ;;
  esac
  SEAMWARE_PLUGIN_DIR="$P" "$d/src/app/coraine/coraine" \
      --port "$PORT" "${dbArgs[@]}" --troe none > "$d.run.log" 2>&1 &
  pid=$!
  echo "$pid" >> "$WORK/pids"

  up=no
  for _ in $(seq 1 30); do
    curl -sf -o /dev/null "http://localhost:$PORT/version" && { up=yes; break; }
    sleep 0.5
  done

  if [ "$up" != yes ]; then
    echo "$label: did not START"
    tail -5 "$d.run.log"
    kill "$pid" 2>/dev/null
    rc=1
    continue
  fi

  #
  # The binary's own account of itself, which is the point: a build whose flag
  # did not take starts just as happily as one whose flag did.
  #
  # One request, and split on commas first: the broker answers /build as ONE
  # LINE unless asked to pretty-print, so a plain grep for a member name matches
  # the whole document and hands back every boolean in it.
  build=$(curl -sf "http://localhost:$PORT/build" | tr ',' '\n')
  regs=$(echo "$build" | grep '"REGISTRATIONS"' | grep -oE 'true|false')
  subs=$(echo "$build" | grep '"SUBSCRIPTIONS"' | grep -oE 'true|false')
  loaded=$(curl -sf "http://localhost:$PORT/build" | python3 -c 'import json,sys; print(json.load(sys.stdin)["plugins"]["loaded"]["currentState"]["alias"])' 2>/dev/null)

  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null

  #
  # The TRoE switches: what the broker offers as --troe (its usage, from the plugins there and its own build) -
  # and --troe corDB refused by a broker built without it, whatever corDB.so is there
  #
  troe=""
  case "$label" in
    TROE_*)
      troe=" troe=$(SEAMWARE_PLUGIN_DIR="$P" "$d/src/app/coraine/coraine" -u 2>&1 | sed -n 's/.*\[--troe\/-troe <\([^>]*\)>\].*/\1/p')"
      if [ "$label" = TROE_CORDB=OFF ]; then
        make -C "$SRC/../corDB" OBJDIR="$d.corDB.full" CORAINE="$SRC" > "$d.corDB.full.log" 2>&1   # a corDB.so WITH history
        cp -p "$d.corDB.full/corDB.so" "$P/db/currentState/" 2>/dev/null
        refused=$(SEAMWARE_PLUGIN_DIR="$P" timeout 10 "$d/src/app/coraine/coraine" --port "$PORT" --database corDB --troe corDB 2>&1 | grep -c "built without corDB's history")
        troe="$troe corDB-refused=$refused"
      fi ;;
  esac

  #
  # The current-state stores: the one a start without --database runs, what --database offers - and a store the
  # broker is built without refused by its name, whatever .so of that name is there (empty files here: the
  # refusal comes before the file is opened, and without it the file would be opened)
  #
  stores=""
  case "$label" in
    MONGOC=OFF|CORDB=OFF)
      for f in db/currentState/corDB.so db/currentState/ramDB.so db/currentState/mongoc.so troe/temporal/ramDB.so; do
        [ -f "$P/$f" ] || : > "$P/$f"
      done
      usage=$(SEAMWARE_PLUGIN_DIR="$P" "$d/src/app/coraine/coraine" -u 2>&1)
      stores=" default=$loaded database=$(echo "$usage" | sed -n 's/.*\[--database\/-db <\([^>]*\)>\].*/\1/p')"
      stores="$stores troe=$(echo "$usage" | sed -n 's/.*\[--troe\/-troe <\([^>]*\)>\].*/\1/p')"
      if [ "$label" = CORDB=OFF ]; then refusedV=("--database corDB" "--database ramDB" "--troe ramDB"); else refusedV=("--database mongoc"); fi
      stores="$stores, refused:"
      for r in "${refusedV[@]}"; do
        # $r unquoted: an option and its value
        SEAMWARE_PLUGIN_DIR="$P" timeout 10 "$d/src/app/coraine/coraine" --port "$PORT" $r "${dbArgs[@]}" 2>&1 | grep -q "this broker was built without" \
          && stores="$stores $r: yes," || stores="$stores $r: no,"
      done
      stores=${stores%,} ;;
  esac

  echo "$label: build ok, start ok, REGISTRATIONS=$regs SUBSCRIPTIONS=$subs$troe$stores"
done

exit $rc
