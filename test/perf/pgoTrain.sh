#!/bin/bash
#
# pgoTrain.sh <coraine> <build-plugins-dir> - the training run of a profile-guided build (`make pgo`)
#
# Runs an instrumented broker (-fprofile-generate) through what it is measured on, so the profile is
# the shape of the work that matters: perfRun's request shapes on one core (short), a write that
# notifies, and the three-broker forwarding chain over HTTP, cor:// between, and cor:// end to end.
# Every broker is stopped with SIGTERM - a clean exit is what writes the profile.
#
# Portable: nothing pinned (a profile needs no pinning, and a Docker build or a 4-core CI runner has
# no CPU 8-15 for corChain.sh's taskset). Needs curl and wrk; corRequest and corTestClient from corLibs.
#
# <build-plugins-dir>: the plugins of the SAME instrumented build (BUILD_PGO/src/plugins) - laid out
# here as the broker loads them; corDB's from its own repo (../corDB, built release and instrumented by
# `make pgo` before this runs).
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
CORDB=${COR_DB_DIR:-$(cd "$HERE/../../../corDB" && pwd)}
cp "$CORDB/obj/release/corDB.so"      "$TMP/plugins/db/currentState/"
cp "$SRC/temporal/none/none.so"       "$TMP/plugins/troe/temporal/"
export SEAMWARE_PLUGIN_DIR=$TMP/plugins

echo "pgoTrain: request shapes (perfRun, short)"
PERF_DURATION=2s PERF_REPEATS=1 PERF_BROKER_CMD="$B --port 1029 --database corDB --troe none --httpLoops 1" \
  "$HERE/perfRun.sh" corDB 1029 > /dev/null 2>&1 || echo "pgoTrain: perfRun failed - the profile lacks the request shapes"

#
# The same shapes on corDB on disk with its history: the log, the snapshots and the history writes are
# what corDB's numbers are mostly made of, and a path the training never runs gets no profile
# (-fprofile-partial-training compiles it as plain -O2 - not cold, but not tuned either)
#
echo "pgoTrain: request shapes on corDB on disk, with history (perfRun, short)"
PERF_DURATION=2s PERF_REPEATS=1 PERF_DB_DIR=$TMP/dbDir PERF_TROE=corDB \
  PERF_BROKER_CMD="$B --port 1029 --database corDB --dbDir $TMP/dbDir --troe corDB --httpLoops 1" \
  "$HERE/perfRun.sh" corDB 1029 > /dev/null 2>&1 || echo "pgoTrain: perfRun on disk failed - the profile lacks the log and the history"

echo "pgoTrain: writes that notify"
TOOLS=${COR_TOOLS_DIR:-$(cd "$HERE/../../../corLibs/bin" && pwd)}
RECEIVER=$TOOLS/corTestClient
"$RECEIVER" --port 7799 --traceLevels "" > /dev/null 2>&1 & R=$!
"$B" --port 1029 --database corDB --troe none --foreground > /dev/null 2>&1 & P=$!
for i in $(seq 1 50); do curl -s -o /dev/null localhost:1029/version && break; sleep 0.1; done
J='Content-Type: application/json'
curl -s -o /dev/null -X POST localhost:1029/ngsi-ld/v1/subscriptions -H "$J" -d '{"id":"urn:S1","type":"Subscription","entities":[{"type":"Vehicle"}],"notification":{"endpoint":{"uri":"http://localhost:7799/notify"}}}'
for i in $(seq 1 100); do curl -s -o /dev/null -X POST localhost:1029/ngsi-ld/v1/entities -H "$J" -d '{"id":"urn:ngsi-ld:Vehicle:'$i'","type":"Vehicle","speed":{"type":"Property","value":1}}'; done
wrk -t4 -c50 -d3s -s "$HERE/patchAttr.lua" http://localhost:1029 > /dev/null 2>&1
kill $P; wait $P 2>/dev/null
kill $R; wait $R 2>/dev/null

#
# The three-broker chain - A -> B -> C, a GET on A forwarded to C - its forwarding hops over HTTP and
# over cor://, its client HTTP and cor://
#
J='Content-Type: application/json'
start() { "$B" --port $1 --corPort $2 --database corDB --troe none -dist --foreground > /dev/null 2>&1 & echo $!; }
for hops in http cor; do
  echo "pgoTrain: the three-broker chain, forwarding over $hops"
  PA=$(start 9201 9301); PB=$(start 9202 9302); PC=$(start 9203 9303)
  for p in 9201 9202 9203; do for i in $(seq 1 50); do curl -s -o /dev/null http://localhost:$p/version && break; sleep 0.1; done; done
  curl -s -o /dev/null -X POST localhost:9203/ngsi-ld/v1/entities -H "$J" -d '{"id":"urn:E1","type":"Vehicle","speed":{"type":"Property","value":42,"observedAt":"2026-10-01T12:00:00.000Z"},"name":{"type":"LanguageProperty","languageMap":{"en":"truck"}},"location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[-3.7,40.4]}}}'
  if [ $hops = cor ]; then EPB=cor://localhost:9302; EPC=cor://localhost:9303; else EPB=http://localhost:9202; EPC=http://localhost:9203; fi
  curl -s -o /dev/null -X POST localhost:9201/ngsi-ld/v1/csourceRegistrations -H "$J" -d '{"id":"urn:Reg:A","type":"ContextSourceRegistration","endpoint":"'$EPB'","mode":"inclusive","information":[{"entities":[{"type":"Vehicle"}]}]}'
  curl -s -o /dev/null -X POST localhost:9202/ngsi-ld/v1/csourceRegistrations -H "$J" -d '{"id":"urn:Reg:B","type":"ContextSourceRegistration","endpoint":"'$EPC'","mode":"inclusive","information":[{"entities":[{"type":"Vehicle"}]}]}'
  wrk -t2 -c1  -d2s http://localhost:9201/ngsi-ld/v1/entities/urn:E1 > /dev/null 2>&1
  wrk -t2 -c16 -d3s http://localhost:9201/ngsi-ld/v1/entities/urn:E1 > /dev/null 2>&1
  if [ $hops = cor ]; then
    "$TOOLS/corRequest" --url cor://localhost:9301 --path /ngsi-ld/v1/entities/urn:E1 -c 1  --duration 2 > /dev/null < /dev/null
    "$TOOLS/corRequest" --url cor://localhost:9301 --path /ngsi-ld/v1/entities/urn:E1 -c 16 --duration 3 > /dev/null < /dev/null
  fi
  #
  # Not this shell's children (started in $(...)): no `wait` - until they are gone, which is when their
  # profile is written
  #
  kill $PA $PB $PC
  for i in $(seq 1 100); do kill -0 $PA 2>/dev/null || kill -0 $PB 2>/dev/null || kill -0 $PC 2>/dev/null || break; sleep 0.1; done
done

exit 0
