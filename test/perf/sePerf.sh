#!/usr/bin/env bash
#
# sePerf.sh - Service Execution throughput: services/s and actions/s, per core (doc/performance.md)
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
#   test/perf/sePerf.sh <store>        store: corDB | mongo   (mongo: MongoDB + TimescaleDB for TRoE)
#
# One broker, the loopback bridge its own executor (loopback://inline: "executing" + progress 50 %, then
# "completed", reported on the broker's thread - no transport, no sleep): what Service Execution costs in the
# broker itself and nothing else. TRoE on.
#
#   service   svc, synchronous   - POST .../services/svc -> 200, the output
#   action    act, asynchronous  - POST .../services/act -> 202; the execution executing, its progress, completed
#
# The broker on PERF_BROKER_CORES physical cores (default 1), wrk on the others (SMT siblings never shared -
# perfRun.sh says why). Every measurement starts from an EMPTY store (a fresh --dbDir / database): every
# execution is stored, so a store that grew in the run before would make the later runs slower.
#
# Per core = the 2xx answers / the CPU-seconds the broker used during the run (utime + stime of the process,
# /proc/<pid>/stat) - not wall time x cores: a broker that does not saturate its core is not flattered.
#
# Env: PERF_DURATION (10s) PERF_REPEATS (3) PERF_CONCURRENCY ("16 64 256") PERF_ENTITIES (100) PERF_PORT (1029)
#      PERF_THREADS (8)  PERF_PG_* (mongo: the TimescaleDB)
#      PERF_SE_STORE_ARGS (corDB: the store arguments instead of --database corDB --dbDir ... --troe corDB, to
#      take the store or TRoE out of the picture)
#
set -u
STORE=${1:-corDB}
PORT=${PERF_PORT:-1029}
DURATION=${PERF_DURATION:-10s}
REPEATS=${PERF_REPEATS:-3}
CONCURRENCY=${PERF_CONCURRENCY:-16 64 256}
ENTITIES=${PERF_ENTITIES:-100}
THREADS=${PERF_THREADS:-8}
CORES=${PERF_BROKER_CORES:-1}
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
WORK=${PERF_WORK:-/tmp/sePerf.$$}
B=http://localhost:$PORT/ngsi-ld/v1
export PERF_ENTITIES=$ENTITIES

if pgrep -x chrome >/dev/null || pgrep -x chromium >/dev/null; then echo "Chrome is running - no perf"; exit 1; fi

# one logical CPU per physical core, from sysfs
mapfile -t phys < <(for d in /sys/devices/system/cpu/cpu[0-9]*; do id=${d##*/cpu}; sib=$(cut -d, -f1 < "$d/topology/thread_siblings_list"); [ "$sib" = "$id" ] && echo "$id"; done | sort -n)
brokerCpus=$(IFS=,; echo "${phys[*]:0:CORES}")
loadCpus=$(IFS=,; echo "${phys[*]:CORES}")

case "$STORE" in
  corDB) storeArgs() { echo "${PERF_SE_STORE_ARGS:---database corDB --dbDir $WORK/db --troe corDB}"; } ;;
  mongo) storeArgs() { echo "--database mongoc --dbHost localhost --dbName seperf --troe timescale ${PERF_PG_ARGS:-}"; } ;;
  *)     echo "sePerf.sh: store corDB or mongo" >&2; exit 1 ;;
esac

pid=""
stopBroker() { [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null; pid=""; }
trap stopBroker EXIT

emptyStore() {
  rm -rf "$WORK"; mkdir -p "$WORK/db"
  if [ "$STORE" = mongo ]; then
    mongosh --quiet --eval 'db.getSiblingDB("seperf").dropDatabase()' >/dev/null 2>&1 || mongo --quiet --eval 'db.getSiblingDB("seperf").dropDatabase()' >/dev/null
    [ -n "${PERF_PG_RESET:-}" ] && eval "$PERF_PG_RESET"
  fi
}

startBroker() {
  emptyStore
  # shellcheck disable=SC2046
  taskset -c "$brokerCpus" coraine --port "$PORT" $(storeArgs) --bridges loopback > "$WORK/broker.log" 2>&1 &
  pid=$!
  for _ in $(seq 50); do curl -s -o /dev/null "http://localhost:$PORT/version" && break; sleep 0.1; done
  # the fixture: ENTITIES robots, the two services
  local batch="[" i
  for i in $(seq "$ENTITIES"); do batch+="{\"id\":\"urn:ngsi-ld:Robot:$i\",\"type\":\"Robot\",\"battery\":{\"type\":\"Property\",\"value\":90}},"; done
  batch="${batch%,}]"
  ok() { [ "$1" = "$2" ] || { echo "sePerf.sh: fixture: $3 answered $1" >&2; cat "$WORK/broker.log" >&2; exit 1; }; }
  ok "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$B/entityOperations/create" -H 'Content-Type: application/json' -d "$batch")" 201 "the robots"
  for s in svc:synchronous act:asynchronous; do
    ok "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$B/serviceRegistrations" -H 'Content-Type: application/json' \
      -d "{\"type\":\"ServiceRegistration\",\"endpoint\":\"loopback://inline\",\"entities\":[{\"type\":\"Robot\"}],\"serviceInformation\":{\"serviceName\":\"${s%%:*}\",\"mode\":\"${s##*:}\"}}")" 201 "registration ${s%%:*}"
  done
}

ticks() { awk '{ sub(/^.*\) /, ""); print $12 + $13 }' "/proc/$pid/stat"; }
HZ=$(getconf CLK_TCK)

measure() {   # measure <svc|act> <connections>  ->  "rate cpu_s per_core p50_us p99_us count"
  local svc=$1 c=$2 t0 t1 out
  startBroker
  # a short warm-up, not counted
  PERF_SE_SERVICE=$svc taskset -c "$loadCpus" wrk -t"$THREADS" -c"$c" -d2s -s "$SCRIPTDIR/serviceExec.lua" "http://localhost:$PORT" >/dev/null 2>&1
  t0=$(ticks)
  out=$(PERF_SE_SERVICE=$svc taskset -c "$loadCpus" wrk -t"$THREADS" -c"$c" -d"$DURATION" -s "$SCRIPTDIR/serviceExec.lua" "http://localhost:$PORT" 2>&1)
  t1=$(ticks)
  if printf '%s' "$out" | grep -q "Non-2xx"; then echo "sePerf.sh: error answers in $svc c=$c:" >&2; printf '%s\n' "$out" >&2; exit 1; fi
  # the actions really ended: none left pending or executing
  if [ "$svc" = act ]; then
    local open
    open=$(curl -s "$B/services?serviceName=act&executionStatus=executing" | head -c 2)
    [ "$open" = "[]" ] || echo "sePerf.sh: WARNING - actions still executing after the run" >&2
  fi
  stopBroker
  printf '%s' "$out" | awk -v cpu="$(( t1 - t0 ))" -v hz="$HZ" '
    / requests in / { n = $1 }
    /Requests\/sec/ { rps = $2 }
    /^PCTL / { p50 = $2; p99 = $4 }
    END { s = cpu / hz; printf "%.0f %.2f %.0f %d %d %d\n", rps, s, (s > 0 ? n / s : 0), p50, p99, n }'
}

echo "# sePerf.sh $STORE - $(date -Is) - coraine $(coraine --version 2>&1 | head -1) - broker on CPU $brokerCpus, wrk on $loadCpus"
echo "# duration $DURATION, $REPEATS repeats, $ENTITIES entities, wrk -t$THREADS; TRoE on; executor loopback://inline"
echo "# kind conc  rate/s  cpu_s  per_core/s  p50_us  p99_us  count"
for r in $(seq "$REPEATS"); do
  # the order turned every repeat (svc act, act svc ...): no kind always first
  if [ $((r % 2)) = 1 ]; then kinds="svc act"; else kinds="act svc"; fi
  for c in $CONCURRENCY; do
    for k in $kinds; do
      printf '%s %4d  ' "$k" "$c"
      measure "$k" "$c"            # not in $( ): a fixture failure exits THIS shell, and the trap stops the broker
    done
  done
done
