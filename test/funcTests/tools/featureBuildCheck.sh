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
# Not run by the ordinary suite: five cmake builds is minutes, against two
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
# Only the flags that actually DO something are here. Nine of the fifteen
# features are declared and unread (see doc/building.md), so a case for one of
# them would build a byte-identical binary and assert nothing; and three do not
# link at all, which is a known state rather than something to pin.
#
CONFIGS=(
  "REGISTRATIONS=OFF|-DCOR_FEATURE_REGISTRATIONS=OFF"
  "SUBSCRIPTIONS=OFF|-DCOR_FEATURE_SUBSCRIPTIONS=OFF"
  "REGISTRATIONS+SUBSCRIPTIONS=OFF|-DCOR_FEATURE_REGISTRATIONS=OFF -DCOR_FEATURE_SUBSCRIPTIONS=OFF"
  "MONGOC=OFF|-DCOR_FEATURE_MONGOC=OFF"
  "ADMIN_API=OFF|-DCOR_FEATURE_ADMIN_API=OFF"
)

rc=0

for cfg in "${CONFIGS[@]}"; do
  label="${cfg%%|*}"
  flags="${cfg#*|}"
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
  P="$d.plugins"
  mkdir -p "$P/db/currentState" "$P/troe/temporal" "$P/api"
  cp -p "$d/src/plugins/currentState/corDB/corDB.so" "$P/db/currentState/" 2>/dev/null
  cp -p "$d/src/plugins/temporal/none/none.so"       "$P/troe/temporal/"   2>/dev/null

  SEAMWARE_PLUGIN_DIR="$P" "$d/src/app/coraine/coraine" \
      --port "$PORT" -fg --database corDB --troe none > "$d.run.log" 2>&1 &
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

  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null

  echo "$label: build ok, start ok, REGISTRATIONS=$regs SUBSCRIPTIONS=$subs"
done

exit $rc
