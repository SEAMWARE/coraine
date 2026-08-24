#!/usr/bin/env bash
#
# perfRun.sh - measure a few fixed request shapes and print one JSON object.
#
# Fixed on purpose: the point is comparability across runs, not coverage. Change a
# scenario and the history before it becomes meaningless, so add scenarios rather
# than editing them.
#
# RELEASE builds only. A debug broker measures the debug broker.
#
# Usage:  perfRun.sh <db> [port]
#   db:   mongoc | corDB
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

DB=${1:?usage: perfRun.sh <mongoc|corDB> [port]}
PORT=${2:-1029}
HOST=${COR_MONGO_HOST:-localhost}
ENTITIES=${PERF_ENTITIES:-100}
DURATION=${PERF_DURATION:-5s}
THREADS=${PERF_THREADS:-8}
REPEATS=${PERF_REPEATS:-3}

case "$DB" in
  mongoc) dbArgs="--database mongoc --dbHost $HOST --dbName corperf" ;;
  corDB)  dbArgs="--database corDB" ;;
  *)      echo "perfRun.sh: unknown db '$DB'" >&2; exit 1 ;;
esac

command -v wrk >/dev/null || { echo "perfRun.sh: wrk is not installed" >&2; exit 1; }

#
# The caller measures one database after another on the SAME port, and a broker
# does not release its port the instant it is signalled: SIGTERM starts a
# shutdown that closes the store and the database connection first. The nightly
# runs corDB and then mongoc, and the second broker reached MHD_start_daemon
# while the first still held the port:
#
#   corRestInit: MHD_start_daemon failed on port 1029
#
# It exited, the readiness probe below then talked to the DYING first broker,
# and the fixture loop's first POST was reset mid-request - which is curl's exit
# 56, and with `set -e` that is what the whole run exited with. No message: the
# caller reads this script's stdout as the result, so anything printed there is
# swallowed rather than shown.
#
# Both halves of the handover are fixed: this run waits for the port before it
# starts, and gives it back before it exits. Diagnostics go to stderr.
#
portInUse() { (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; }

awaitPortFree() {
  local i
  for i in $(seq 1 100); do
    portInUse || return 0
    sleep 0.1
  done
  echo "perfRun.sh: port $PORT is still in use after 10s - somebody else is on it" >&2
  return 1
}

stopBroker() {
  local i
  kill "$brokerPid" 2>/dev/null || return 0
  for i in $(seq 1 100); do
    kill -0 "$brokerPid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$brokerPid" 2>/dev/null || true
  awaitPortFree || true
}

awaitPortFree

coraine --port "$PORT" $dbArgs --troe none > /tmp/perf-broker.log 2>&1 &
brokerPid=$!
trap stopBroker EXIT

for i in $(seq 1 60); do
  curl -sf "http://localhost:$PORT/ngsi-ld/v1/types" > /dev/null && break
  #
  # Ask the process, not the port. A readiness probe that only asks "does
  # something answer on $PORT" passes against a broker this script did not
  # start, and then every number below belongs to the wrong binary.
  #
  kill -0 "$brokerPid" 2>/dev/null \
    || { echo "perfRun.sh: the broker exited during startup" >&2; cat /tmp/perf-broker.log >&2; exit 1; }
  sleep 0.5
  [ "$i" = 60 ] && { echo "perfRun.sh: broker never came up" >&2; cat /tmp/perf-broker.log >&2; exit 1; }
done

#
# The fixture: five-attribute entities of ~550 bytes, so a limit=20 query returns
# about 11 KB - a realistic page rather than a toy.
#
for i in $(seq 1 "$ENTITIES"); do
  curl -s -o /dev/null -X POST "http://localhost:$PORT/ngsi-ld/v1/entities" \
    -H 'Content-Type: application/json' \
    -d "{\"id\":\"urn:ngsi-ld:Vehicle:$i\",\"type\":\"Vehicle\",
         \"brand\":{\"type\":\"Property\",\"value\":\"Mercedes\"},
         \"speed\":{\"type\":\"Property\",\"value\":$((i % 120)),\"observedAt\":\"2026-08-20T10:00:00Z\"},
         \"location\":{\"type\":\"GeoProperty\",\"value\":{\"type\":\"Point\",\"coordinates\":[13.4,52.5]}},
         \"isParked\":{\"type\":\"Relationship\",\"object\":\"urn:ngsi-ld:OffStreetParking:$i\"},
         \"description\":{\"type\":\"Property\",\"value\":\"a five-attribute vehicle used for throughput measurement, padded to roughly five hundred bytes so the numbers mean something ------------------------------------------------\"}}"
done

# median of REPEATS runs - a single wrk run on a shared runner is a rumour
measure() {
  local url="$1" conns="$2" rpsList=()
  wrk -t"$THREADS" -c"$conns" -d2s "$url" > /dev/null 2>&1        # warmup
  for _ in $(seq 1 "$REPEATS"); do
    rpsList+=( "$(wrk -t"$THREADS" -c"$conns" -d"$DURATION" "$url" 2>/dev/null | awk '/Requests\/sec/{printf "%.0f", $2}')" )
  done
  printf '%s\n' "${rpsList[@]}" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'
}

queryC50=$(measure "http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=20" 50)
queryC200=$(measure "http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=20" 200)
retrieve=$(measure  "http://localhost:$PORT/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:7" 50)

printf '{"db":"%s","query_c50":%s,"query_c200":%s,"retrieve_c50":%s}\n' \
       "$DB" "$queryC50" "$queryC200" "$retrieve"
