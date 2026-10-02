#!/bin/bash
#
# pgoTrain.sh <coraine> <build-plugins-dir> - the training run of a profile-guided build (`make pgo`)
#
# Runs an instrumented broker (-fprofile-generate) through what it is measured on, so the profile is
# the shape of the work that matters: perfRun's request shapes on one core (short), a write that
# notifies, and the three-broker forwarding chain over HTTP, cor:// between, and cor:// end to end.
# Every broker is stopped with SIGTERM - a clean exit is what writes the profile.
#
# <build-plugins-dir>: the plugins of the SAME instrumented build (BUILD_PGO/src/plugins) - laid out
# here as the broker loads them.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u
B=$(realpath "$1")
SRC=$(realpath "$2")
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/plugins/db/currentState" "$TMP/plugins/troe/temporal" "$TMP/plugins/api" "$TMP/plugins/bridge"
cp "$SRC/currentState/corDB/corDB.so" "$TMP/plugins/db/currentState/"
cp "$SRC/temporal/none/none.so"       "$TMP/plugins/troe/temporal/"
export SEAMWARE_PLUGIN_DIR=$TMP/plugins

echo "pgoTrain: request shapes (perfRun, short)"
PERF_DURATION=2s PERF_REPEATS=1 PERF_BROKER_CORES=1 PERF_BROKER_CMD="$B --port 1029 --database corDB --troe none --httpLoops 1" \
  "$HERE/perfRun.sh" corDB 1029 > /dev/null 2>&1 || echo "pgoTrain: perfRun failed - the profile lacks the request shapes"

echo "pgoTrain: writes that notify"
RECEIVER=${CORTESTCLIENT:-$(dirname "$HERE")/../../corLibs/bin/corTestClient}
"$RECEIVER" --port 7799 --traceLevels "" > /dev/null 2>&1 & R=$!
"$B" --port 1029 --database corDB --troe none --foreground > /dev/null 2>&1 & P=$!
for i in $(seq 1 50); do curl -s -o /dev/null localhost:1029/version && break; sleep 0.1; done
J='Content-Type: application/json'
curl -s -o /dev/null -X POST localhost:1029/ngsi-ld/v1/subscriptions -H "$J" -d '{"id":"urn:S1","type":"Subscription","entities":[{"type":"Vehicle"}],"notification":{"endpoint":{"uri":"http://localhost:7799/notify"}}}'
for i in $(seq 1 100); do curl -s -o /dev/null -X POST localhost:1029/ngsi-ld/v1/entities -H "$J" -d '{"id":"urn:ngsi-ld:Vehicle:'$i'","type":"Vehicle","speed":{"type":"Property","value":1}}'; done
wrk -t4 -c50 -d3s -s "$HERE/patchAttr.lua" http://localhost:1029 > /dev/null 2>&1
kill $P; wait $P 2>/dev/null
kill $R; wait $R 2>/dev/null

for mode in http cor cor-all; do
  echo "pgoTrain: the three-broker chain, $mode"
  CORAINE_BIN="$B" "$HERE/corChain.sh" $mode > /dev/null 2>&1
done
