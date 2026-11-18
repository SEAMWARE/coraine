#!/usr/bin/env bash
#
# sePerf2.sh - Service Execution through a second broker: services/s and actions/s, per core (doc/performance.md)
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
#   test/perf/sePerf2.sh <store> <transport>     store: corDB | mongo     transport: cor | http
#
# Two brokers, TRoE on in both (corDB: the store's own history; mongo: MongoDB + TimescaleDB):
#
#   A - the clients' broker: the robots and their services, whose executor is B's executor route
#       (doc/service-execution.md, "Another broker as the executor"), over cor:// or http://
#   B - the executor: the same robots, the services registered to its loopback bridge (loopback://inline:
#       "executing" + progress 50 %, then "completed", on B's own thread), and every change of an
#       action's execution reported back to A - PATCH /ngsi-ld/v1/services/{id}, over the same transport
#
#   service   svc, synchronous   - POST A .../services/svc -> A invokes it on B -> 200, the output
#   action    act, asynchronous  - POST A .../services/act -> A hands it to B -> 202; B reports to A executing,
#                                  progress, completed. An action counts when it is COMPLETED on A: the rate is
#                                  the 202s over the time from the start until A has none executing any more
#
# A on the first physical core, B on the second, wrk on the others (one logical CPU per physical core).
# Rate = the calls / (wrk's run time + the drain: from wrk's end until A had no action executing).
# Per core = the completed calls / the CPU-seconds of A and B together (utime + stime, /proc/<pid>/stat,
# until the last action completed) - two brokers on two cores: per core is what a core of the pair does.
# Every measurement starts from EMPTY stores (fresh --dbDir / databases).
#
# Env: PERF_DURATION (10s) PERF_REPEATS (3) PERF_CONCURRENCY ("16 64 256") PERF_ENTITIES (100)
#      PERF_THREADS (8) PERF_PORT (1029: A; B is +1; cor:// ports +1000)
#      PERF_A_ARGS, PERF_B_ARGS - more options for A, for B (e.g. --connectionPoolSize 32)
#
set -u
STORE=${1:-corDB}
TRANSPORT=${2:-cor}
PORT_A=${PERF_PORT:-1029}
PORT_B=$((PORT_A + 1))
COR_A=$((PORT_A + 1000))
COR_B=$((PORT_B + 1000))
DURATION=${PERF_DURATION:-10s}
REPEATS=${PERF_REPEATS:-3}
CONCURRENCY=${PERF_CONCURRENCY:-16 64 256}
ENTITIES=${PERF_ENTITIES:-100}
THREADS=${PERF_THREADS:-8}
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
WORK=${PERF_WORK:-/tmp/sePerf2.$$}
A=http://localhost:$PORT_A/ngsi-ld/v1
B=http://localhost:$PORT_B/ngsi-ld/v1
export PERF_ENTITIES=$ENTITIES

case "$TRANSPORT" in
  cor)  executor="cor://localhost:$COR_B";   reportTo="cor://localhost:$COR_A" ;;
  http) executor="http://localhost:$PORT_B"; reportTo="http://localhost:$PORT_A" ;;
  *)    echo "sePerf2.sh: transport cor or http" >&2; exit 1 ;;
esac

if pgrep -x chrome >/dev/null || pgrep -x chromium >/dev/null; then echo "Chrome is running - no perf"; exit 1; fi

mapfile -t phys < <(for d in /sys/devices/system/cpu/cpu[0-9]*; do id=${d##*/cpu}; sib=$(cut -d, -f1 < "$d/topology/thread_siblings_list"); [ "$sib" = "$id" ] && echo "$id"; done | sort -n)
cpuA=${phys[0]}
cpuB=${phys[1]}
loadCpus=$(IFS=,; echo "${phys[*]:2}")

storeArgs() {   # storeArgs <a|b>
  case "$STORE" in
    corDB) echo "--database corDB --dbDir $WORK/db$1 --troe corDB" ;;
    mongo) echo "--database mongoc --dbHost localhost --dbName seperf2$1 --troe timescale --troeName seperf2$1" ;;
    *)     echo "sePerf2.sh: store corDB or mongo" >&2; exit 1 ;;
  esac
}

pidA=""
pidB=""
stopBrokers() {
  for p in $pidA $pidB; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
  pidA=""; pidB=""
}
trap stopBrokers EXIT

emptyStores() {
  rm -rf "$WORK"; mkdir -p "$WORK/dba" "$WORK/dbb"
  if [ "$STORE" = mongo ]; then
    for x in a b; do
      mongosh --quiet --eval "db.getSiblingDB('seperf2$x').dropDatabase()" >/dev/null 2>&1 || mongo --quiet --eval "db.getSiblingDB('seperf2$x').dropDatabase()" >/dev/null
      psql -h localhost -U postgres -d postgres -qc "DROP DATABASE IF EXISTS seperf2$x WITH (FORCE)" >/dev/null
    done
  fi
}

ok() { [ "$1" = "$2" ] || { echo "sePerf2.sh: fixture: $3 answered $1" >&2; tail -n 20 "$WORK"/*.log >&2; exit 1; }; }

startBrokers() {
  emptyStores
  # shellcheck disable=SC2046
  taskset -c "$cpuB" coraine --port "$PORT_B" --corPort "$COR_B" $(storeArgs b) --bridges loopback ${PERF_B_ARGS:-} > "$WORK/B.log" 2>&1 &
  pidB=$!
  # shellcheck disable=SC2046
  taskset -c "$cpuA" coraine --port "$PORT_A" --corPort "$COR_A" $(storeArgs a) ${PERF_A_ARGS:-} > "$WORK/A.log" 2>&1 &
  pidA=$!
  for p in $PORT_A $PORT_B; do
    for _ in $(seq 100); do curl -s -o /dev/null "http://localhost:$p/version" && break; sleep 0.1; done
  done

  local batch="[" i
  for i in $(seq "$ENTITIES"); do batch+="{\"id\":\"urn:ngsi-ld:Robot:$i\",\"type\":\"Robot\",\"battery\":{\"type\":\"Property\",\"value\":90}},"; done
  batch="${batch%,}]"
  ok "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$A/entityOperations/create" -H 'Content-Type: application/json' -d "$batch")" 201 "the robots on A"
  ok "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$B/entityOperations/create" -H 'Content-Type: application/json' -d "$batch")" 201 "the robots on B"
  for s in svc:synchronous act:asynchronous; do
    ok "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$B/serviceRegistrations" -H 'Content-Type: application/json' \
      -d "{\"type\":\"ServiceRegistration\",\"endpoint\":\"loopback://inline\",\"entities\":[{\"type\":\"Robot\"}],\"serviceInformation\":{\"serviceName\":\"${s%%:*}\",\"mode\":\"${s##*:}\"}}")" 201 "registration ${s%%:*} on B"
    ok "$(curl -s -o /dev/null -w '%{http_code}' -X POST "$A/serviceRegistrations" -H 'Content-Type: application/json' \
      -d "{\"type\":\"ServiceRegistration\",\"endpoint\":\"$executor/ngsi-ld/ex/v1/executor/${s%%:*}?reportTo=$reportTo\",\"entities\":[{\"type\":\"Robot\"}],\"serviceInformation\":{\"serviceName\":\"${s%%:*}\",\"mode\":\"${s##*:}\"}}")" 201 "registration ${s%%:*} on A"
  done
}

ticks() { awk '{ sub(/^.*\) /, ""); print $12 + $13 }' "/proc/$1/stat"; }
HZ=$(getconf CLK_TCK)
nowMs() { date +%s%3N; }

executingOnA() { curl -s "$A/services?serviceName=act&executionStatus=executing" | head -c 2; }

measure() {   # measure <svc|act> <connections>  ->  "rate cpu_s per_core p50_us p99_us count drain_ms"
  local svc=$1 c=$2 ta0 tb0 ta1 tb1 t0 t1 out drain=0
  startBrokers
  PERF_SE_SERVICE=$svc taskset -c "$loadCpus" wrk -t"$THREADS" -c"$c" -d2s -s "$SCRIPTDIR/serviceExec.lua" "http://localhost:$PORT_A" >/dev/null 2>&1
  for _ in $(seq 200); do [ "$svc" = svc ] || [ "$(executingOnA)" = "[]" ] && break; sleep 0.05; done   # the warm-up's actions ended
  ta0=$(ticks "$pidA"); tb0=$(ticks "$pidB"); t0=$(nowMs)
  out=$(PERF_SE_SERVICE=$svc taskset -c "$loadCpus" wrk -t"$THREADS" -c"$c" -d"$DURATION" -s "$SCRIPTDIR/serviceExec.lua" "http://localhost:$PORT_A" 2>&1)
  if [ "$svc" = act ]; then
    local tw
    tw=$(nowMs)
    for _ in $(seq 600); do [ "$(executingOnA)" = "[]" ] && break; sleep 0.05; done
    drain=$(( $(nowMs) - tw ))
    [ "$(executingOnA)" = "[]" ] || echo "sePerf2.sh: WARNING - actions still executing on A 30 s after the run" >&2
    [ "$(curl -s "$A/services?serviceName=act&executionStatus=failed" | head -c 2)" = "[]" ] || echo "sePerf2.sh: WARNING - failed actions on A" >&2
  fi
  t1=$(nowMs); ta1=$(ticks "$pidA"); tb1=$(ticks "$pidB")
  [ -n "${PERF_DEBUG:-}" ] && { echo "t0 $t0 t1 $t1"; printf "%s\n" "$out"; } >&2
  if printf '%s' "$out" | grep -q "Non-2xx"; then echo "sePerf2.sh: error answers in $svc c=$c:" >&2; printf '%s\n' "$out" >&2; exit 1; fi
  stopBrokers
  printf '%s' "$out" | awk -v cpu="$(( ta1 - ta0 + tb1 - tb0 ))" -v hz="$HZ" -v ms="$(( t1 - t0 ))" -v drain="$drain" '
    / requests in / { n = $1; secs = $4; sub(/s,?$/, "", secs) }
    /^PCTL / { p50 = $2; p99 = $4 }
    END { s = cpu / hz; t = secs + drain / 1000; printf "%.0f %.2f %.0f %d %d %d %d\n", n / t, s, (s > 0 ? n / s : 0), p50, p99, n, drain }'
}

echo "# sePerf2.sh $STORE $TRANSPORT - $(date -Is) - coraine $(coraine --version 2>&1 | head -1) - A on CPU $cpuA, B on CPU $cpuB, wrk on $loadCpus"
echo "# duration $DURATION, $REPEATS repeats, $ENTITIES entities, wrk -t$THREADS; TRoE on in both; A -> B over $TRANSPORT, B's executor loopback://inline"
echo "# kind conc  rate/s  cpu_s(A+B)  per_core/s  p50_us  p99_us  count  drain_ms"
for r in $(seq "$REPEATS"); do
  if [ $((r % 2)) = 1 ]; then kinds="svc act"; else kinds="act svc"; fi
  for c in $CONCURRENCY; do
    for k in $kinds; do
      printf '%s %4d  ' "$k" "$c"
      measure "$k" "$c"
    done
  done
done
