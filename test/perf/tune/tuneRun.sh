#!/bin/bash
#
# tuneRun.sh <genDir> <coraine> <plugins-dir> [broker options ...] - one run of a workload, one JSON line
#
# <genDir> is tuneGen.py's output for the workload: the fixture, the subscriptions, mix.lua and the store's
# options. The broker starts on an empty store with those options and the ones given here (--dbLockPrefer,
# say), the fixture goes in, the subscriptions are made - notifying a corTestClient --discard, which counts
# them and keeps nothing - and mix.lua runs: TUNE_WARMUP (5s) unmeasured, then TUNE_DURATION (the
# workload's duration, else 30s) measured, at TUNE_CONCURRENCY connections (the workload's).
#
#   {"rps":..., "p50us":..., "p95us":..., "p99us":..., "notified":...}
#
# <plugins-dir>: the build's plugins, laid out as the broker loads them (db/currentState/corDB.so, ...).
# TUNE_PRELOAD: an allocator to LD_PRELOAD into the broker (jemalloc, tcmalloc) - empty: glibc's.
# TUNE_BROKER_CPUS / TUNE_LOAD_CPUS: taskset lists for the broker and for the load (empty: unpinned).
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u
GEN=$(realpath "$1"); B=$(realpath "$2"); PLUGINS=$(realpath "$3"); shift 3

PORT=${TUNE_PORT:-1029}
RPORT=${TUNE_RECEIVER_PORT:-7799}
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=${COR_TOOLS_DIR:-$(cd "$HERE/../../../../corLibs/bin" 2>/dev/null && pwd)}
WARMUP=${TUNE_WARMUP:-5s}
DURATION=${TUNE_DURATION:-30s}
CONNS=${TUNE_CONCURRENCY:-50}
TMP=$(mktemp -d)

pinB=(); pinL=()
[ -n "${TUNE_BROKER_CPUS:-}" ] && pinB=(taskset -c "$TUNE_BROKER_CPUS")
[ -n "${TUNE_LOAD_CPUS:-}" ]   && pinL=(taskset -c "$TUNE_LOAD_CPUS")

cleanup() {
  [ -n "${BP:-}" ] && kill "$BP" 2>/dev/null && wait "$BP" 2>/dev/null
  [ -n "${RP:-}" ] && kill "$RP" 2>/dev/null && wait "$RP" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT
fail() { echo "tuneRun: $*" >&2; exit 1; }

#
# The broker - the store's options from the workload, __DBDIR__ a fresh directory
#
read -r -a storeArgs < "$GEN/broker.args"
for i in "${!storeArgs[@]}"; do [ "${storeArgs[$i]}" = "__DBDIR__" ] && storeArgs[$i]=$TMP/db; done

SEAMWARE_PLUGIN_DIR=$PLUGINS LD_PRELOAD=${TUNE_PRELOAD:-} "${pinB[@]}" \
  "$B" --foreground --port "$PORT" "${storeArgs[@]}" "$@" > "$TMP/broker.out" 2>&1 &
BP=$!
for _ in $(seq 1 100); do curl -s -o /dev/null "http://localhost:$PORT/version" && break; sleep 0.1; done
kill -0 "$BP" 2>/dev/null || fail "the broker did not start: $(head -3 "$TMP/broker.out")"

#
# The receiver, the fixture, the subscriptions
#
"${pinL[@]}" "$TOOLS/corTestClient" --port "$RPORT" --discard --traceLevels "" --foreground > /dev/null 2>&1 &
RP=$!
for _ in $(seq 1 50); do curl -s -o /dev/null "http://localhost:$RPORT/count" && break; sleep 0.1; done

while IFS= read -r line; do
  code=$(printf '%s' "$line" | curl -s -o /dev/null -w '%{http_code}' -X POST "http://localhost:$PORT/ngsi-ld/v1/entityOperations/create" \
                -H 'Content-Type: application/json' --data-binary @-)
  [ "$code" = 201 ] || fail "the fixture got HTTP $code"
done < "$GEN/fixture.ndjson"

while IFS= read -r line; do
  code=$(printf '%s' "${line//__RECEIVER__/http://localhost:$RPORT/notify}" | curl -s -o /dev/null -w '%{http_code}' -X POST \
                "http://localhost:$PORT/ngsi-ld/v1/subscriptions" -H 'Content-Type: application/json' --data-binary @-)
  [ "$code" = 201 ] || fail "a subscription got HTTP $code"
done < "$GEN/subs.ndjson"

#
# The mix - warm-up, then the measured run
#
threads=$(( CONNS < 8 ? CONNS : 8 ))
"${pinL[@]}" wrk -t"$threads" -c"$CONNS" -d"$WARMUP" -s "$GEN/mix.lua" "http://localhost:$PORT" -- w > /dev/null 2>&1
before=$(curl -s "http://localhost:$RPORT/count")
"${pinL[@]}" wrk -t"$threads" -c"$CONNS" -d"$DURATION" -s "$GEN/mix.lua" "http://localhost:$PORT" -- m > "$TMP/wrk.out" 2>&1
after=$(curl -s "http://localhost:$RPORT/count")

rps=$(awk '/Requests\/sec/ { printf "%d", $2 }' "$TMP/wrk.out")
read -r p50 p95 p99 < <(awk '/^PCTL/ { print $2, $3, $4 }' "$TMP/wrk.out")
errs=$(awk '/Non-2xx/ { print $NF }' "$TMP/wrk.out")
[ -n "$rps" ] || fail "wrk measured nothing: $(tail -3 "$TMP/wrk.out")"
firstBad=$(sed -n 's/^FIRSTBAD //p' "$TMP/wrk.out")
[ -n "$firstBad" ] && echo "tuneRun: ${errs:-some} requests failed - the first: $firstBad" >&2

printf '{"rps":%s,"p50us":%s,"p95us":%s,"p99us":%s,"notified":%s,"non2xx":%s}\n' \
  "$rps" "${p50:-0}" "${p95:-0}" "${p99:-0}" "$(( ${after:-0} - ${before:-0} ))" "${errs:-0}"
