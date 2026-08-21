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

coraine --port "$PORT" $dbArgs --troe none > /tmp/perf-broker.log 2>&1 &
brokerPid=$!
trap 'kill $brokerPid 2>/dev/null || true' EXIT

for i in $(seq 1 60); do
  curl -sf "http://localhost:$PORT/ngsi-ld/v1/types" > /dev/null && break
  sleep 0.5
  [ "$i" = 60 ] && { echo "perfRun.sh: broker never came up"; cat /tmp/perf-broker.log; exit 1; }
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
