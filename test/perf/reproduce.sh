#!/usr/bin/env bash
#
# reproduce.sh - measure coraine's throughput yourself, on your own machine
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# Don't take our numbers on trust: this script produces them. It needs nothing
# from us but a published image, and it is meant to be READ before it is run -
# every step below says what it does and why, and there is nothing else to it:
# the load scripts are in this file too.
#
#   Requirements:  Linux, docker, wrk, curl        (apt install wrk / dnf install wrk)
#   Usage:         bash reproduce.sh [image]
#   Example:       bash reproduce.sh quay.io/seamware/coraine:0.4.0
#
#   Environment (all optional):
#     CORES="1 4"     the core counts to measure the broker on
#     DURATION=5      seconds per measured wrk run
#     REPEATS=3       measured runs per figure - the MEDIAN is reported
#     PORT=19026      the port the broker listens on (host network)
#
# What is measured, and what is not:
#
#   - corDB, coraine's in-memory store. Entities live in the broker's own RAM,
#     and there is NO persistence: a restart loses them. These are the numbers of
#     the broker - parsing, validation, storage, rendering, HTTP - with no
#     database server in the picture. A broker writing to MongoDB is bound by
#     MongoDB, and measures MongoDB.
#   - The broker is pinned to N PHYSICAL cores; the load generator (wrk) runs on
#     other cores of the same machine, so it competes for cache and memory
#     bandwidth: if anything, the broker is understated.
#   - Every figure is per the stated core count. Nothing is divided or scaled.
#
# What makes a figure count:
#
#   - every response is a 2xx - a rate measured over error answers is not a rate;
#   - for the write scenarios, the store holds what was acknowledged: after the
#     run, the broker is asked how many entities it has, and that must match the
#     2xx responses wrk counted (times the batch size). A batch answered 207 -
#     some entities refused - would pass the first check and fail this one.
#
# A figure that fails either check is not printed: the script stops and says why.
#
set -euo pipefail

IMAGE=${1:-quay.io/seamware/coraine:0.4.0}
CORES=${CORES:-"1 4"}
DURATION=${DURATION:-5}
REPEATS=${REPEATS:-3}
PORT=${PORT:-19026}
NAME=coraine-reproduce
BASE="http://localhost:$PORT/ngsi-ld/v1"
WORK=$(mktemp -d)

say()  { printf '%s\n' "$*" >&2; }
die()  { say "reproduce.sh: $*"; exit 1; }

cleanup() { docker rm -f "$NAME" > /dev/null 2>&1 || true; rm -rf "$WORK"; }
trap cleanup EXIT



# -----------------------------------------------------------------------------
#
# 1. Preflight - the tools, and the exact image being measured
#
[ "$(uname -s)" = "Linux" ] || die "Linux only: the broker runs with --network host, which Docker Desktop does not give you"
for tool in docker wrk curl; do
  command -v "$tool" > /dev/null || die "'$tool' is not installed"
done

say "Pulling $IMAGE ..."
if ! docker pull -q "$IMAGE" > /dev/null 2>&1; then
  docker image inspect "$IMAGE" > /dev/null 2>&1 || die "cannot pull $IMAGE, and there is no local image of that name"
  say "  (not pulled - using the LOCAL image $IMAGE: fine for checking a build, not a published figure)"
fi
DIGEST=$(docker image inspect --format '{{index .RepoDigests 0}}' "$IMAGE" 2>/dev/null || echo "$IMAGE (local, no digest)")



# -----------------------------------------------------------------------------
#
# 2. Which CPUs - one logical CPU per PHYSICAL core, read from the kernel
#
# SMT is the trap. Two logical CPUs of one core share its execution units, so a
# broker "on 4 CPUs" may be on 2 cores, and a load generator on the SIBLING of a
# broker CPU fights it for the same silicon. So: the broker gets the first N
# physical cores (one logical CPU each, the siblings left idle), and wrk gets
# every logical CPU of the cores after them.
#
# Core 0 is left out: on most Linux machines it takes the bulk of the device
# interrupts - the network's included - so a broker measured on it measures that too.
#
mapfile -t PHYS < <(
  for d in /sys/devices/system/cpu/cpu[0-9]*; do
    id=${d##*/cpu}
    first=$(cut -d, -f1 < "$d/topology/thread_siblings_list" 2>/dev/null | cut -d- -f1 || echo "$id")
    [ "$first" = "$id" ] && echo "$id"
  done | sort -n
)

MAXCORES=0
for n in $CORES; do [ "$n" -gt "$MAXCORES" ] && MAXCORES=$n; done
[ "${#PHYS[@]}" -ge $((MAXCORES + 3)) ] || die "${#PHYS[@]} physical cores: too few to give the broker $MAXCORES, keep core 0 out, and still drive the load"

brokerCpus() { local n=$1; (IFS=,; echo "${PHYS[*]:1:n}"); }

WRK_CPUS=$(
  for c in "${PHYS[@]:MAXCORES+1}"; do
    cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list"
  done | paste -sd, -
)
WRK_THREADS=$(( ${#PHYS[@]} - MAXCORES - 1 ))
[ "$WRK_THREADS" -gt 8 ] && WRK_THREADS=8
CONNS=50



# -----------------------------------------------------------------------------
#
# 3. The requests - the same entity everywhere: five attributes, ~550 bytes
#
cat > "$WORK/create.lua" <<'EOF'
-- POST /entities, one entity per request, a new id every time
local seq, head, tail, threads = 0, nil, nil, 0
function setup(thread) thread:set("slot", threads); threads = threads + 1 end
function init(args)
  head = '{"id":"urn:ngsi-ld:Vehicle:new-' .. (slot or 0) .. '-'
  tail = '","type":"Vehicle","brand":{"type":"Property","value":"Mercedes"},' ..
         '"speed":{"type":"Property","value":42,"observedAt":"2026-08-20T10:00:00Z"},' ..
         '"location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[13.4,52.5]}},' ..
         '"isParked":{"type":"Relationship","object":"urn:ngsi-ld:OffStreetParking:1"},' ..
         '"description":{"type":"Property","value":"a five-attribute vehicle used for throughput measurement, padded to roughly five hundred bytes so the numbers mean something ------------------------------------------------"}}'
end
function request()
  seq = seq + 1
  return wrk.format("POST", "/ngsi-ld/v1/entities", {["Content-Type"] = "application/json"}, head .. seq .. tail)
end
EOF

cat > "$WORK/batch.lua" <<'EOF'
-- POST /entityOperations/create, 20 entities per request, new ids every time
local seq, head, tail, threads, parts = 0, nil, nil, 0, {}
function setup(thread) thread:set("slot", threads); threads = threads + 1 end
function init(args)
  head = '{"id":"urn:ngsi-ld:Vehicle:new-' .. (slot or 0) .. '-'
  tail = '","type":"Vehicle","brand":{"type":"Property","value":"Mercedes"},' ..
         '"speed":{"type":"Property","value":42,"observedAt":"2026-08-20T10:00:00Z"},' ..
         '"location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[13.4,52.5]}},' ..
         '"isParked":{"type":"Relationship","object":"urn:ngsi-ld:OffStreetParking:1"},' ..
         '"description":{"type":"Property","value":"a five-attribute vehicle used for throughput measurement, padded to roughly five hundred bytes so the numbers mean something ------------------------------------------------"}}'
end
function request()
  for i = 1, 20 do seq = seq + 1; parts[i] = head .. seq .. tail end
  return wrk.format("POST", "/ngsi-ld/v1/entityOperations/create", {["Content-Type"] = "application/json"},
                    "[" .. table.concat(parts, ",") .. "]")
end
EOF

fixtureEntity() {
  printf '{"id":"urn:ngsi-ld:Vehicle:%d","type":"Vehicle","brand":{"type":"Property","value":"Mercedes"},"speed":{"type":"Property","value":%d,"observedAt":"2026-08-20T10:00:00Z"},"location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[13.4,52.5]}},"isParked":{"type":"Relationship","object":"urn:ngsi-ld:OffStreetParking:%d"},"description":{"type":"Property","value":"a five-attribute vehicle used for throughput measurement, padded to roughly five hundred bytes so the numbers mean something ------------------------------------------------"}}' "$1" "$(( $1 % 120 ))" "$1"
}



# -----------------------------------------------------------------------------
#
# 4. The broker - a fresh one for every measured run, so a create run never starts
#    on the previous run's entities
#
startBroker() {
  local cpus=$1
  docker rm -f "$NAME" > /dev/null 2>&1 || true
  docker run -d --name "$NAME" --network host --cpuset-cpus "$cpus" "$IMAGE" \
         --port "$PORT" --database corDB --troe none > /dev/null
  for _ in $(seq 1 100); do
    curl -sf "$BASE/types" > /dev/null && return 0
    sleep 0.1
  done
  docker logs "$NAME" >&2 || true
  die "the broker did not come up"
}

stopBroker() { docker rm -f "$NAME" > /dev/null 2>&1 || true; }

storedCount() {
  curl -s -D - -o /dev/null "$BASE/entities?type=Vehicle&limit=0&count=true" |
    awk 'BEGIN{IGNORECASE=1} /^NGSILD-Results-Count:/ {gsub(/\r/,""); print $2}'
}

fixture() {      # 100 entities, for the read scenarios - and checked
  local body="[" i
  for i in $(seq 1 100); do [ "$i" -gt 1 ] && body+=","; body+=$(fixtureEntity "$i"); done
  curl -s -o /dev/null -X POST "$BASE/entityOperations/create" -H 'Content-Type: application/json' --data-binary "$body]"
  [ "$(storedCount)" = 100 ] || die "the fixture did not store 100 entities"
}

#
# A machine still busy with the previous run measures low - up to 13% was seen.
# Wait for the 1-minute load average to fall below 2 (at most two minutes).
#
quiet() {
  for _ in $(seq 1 120); do
    awk '{exit !($1 < 2)}' /proc/loadavg && return 0
    sleep 1
  done
  say "  (the machine never went quiet - load average $(cut -d' ' -f1 /proc/loadavg); measuring anyway)"
}



# -----------------------------------------------------------------------------
#
# 5. One wrk run -> "requests/s p99(us) responses", with the 2xx check
#
wrkRun() {       # wrkRun <seconds> <wrk args...>
  local secs=$1; shift
  local out
  out=$(taskset -c "$WRK_CPUS" wrk --latency -t"$WRK_THREADS" -c"$CONNS" -d"${secs}s" "$@" 2>&1) || true
  if printf '%s' "$out" | grep -q "Non-2xx"; then
    printf '%s\n' "$out" >&2
    die "error responses in: wrk $* - no figure from a run that got errors"
  fi
  printf '%s' "$out" | awk '
    /requests in/  { n = $1 }
    /Requests\/sec/ { rps = $2 }
    /^ *99%/ { v = $2
               if      (v ~ /us$/) { sub(/us$/, "", v); p = v }
               else if (v ~ /ms$/) { sub(/ms$/, "", v); p = v * 1000 }
               else if (v ~ /s$/)  { sub(/s$/,  "", v); p = v * 1000000 } }
    END { if (rps == "") exit 1; printf "%.0f %.0f %d\n", rps, p, n }'
}

median() { sort -n -k1,1 | awk '{a[NR]=$0} END{print a[int((NR+1)/2)]}'; }



# -----------------------------------------------------------------------------
#
# 6. The scenarios
#
# Reads: one broker with the fixture; a 2 s warm-up, then REPEATS runs.
# Writes: a fresh, empty broker per run, and the store is counted afterwards.
#
readScenario() {     # readScenario <cpus> <url>
  local cpus=$1 url=$2 runs=""
  quiet
  startBroker "$cpus"
  fixture
  wrkRun 2 "$url" > /dev/null
  for _ in $(seq 1 "$REPEATS"); do runs+="$(wrkRun "$DURATION" "$url")"$'\n'; done
  stopBroker
  printf '%s' "$runs" | median
}

writeScenario() {    # writeScenario <cpus> <lua> <entities per request>
  local cpus=$1 lua=$2 per=$3 runs="" r n stored
  for _ in $(seq 1 "$REPEATS"); do
    quiet
    startBroker "$cpus"
    r=$(wrkRun "$DURATION" -s "$lua" "http://localhost:$PORT")
    n=$(echo "$r" | cut -d' ' -f3)
    stored=$(storedCount)
    #
    # wrk counts the responses it received; requests still in flight when it
    # stops may land after that - at most one per connection. So the store holds
    # at least every acknowledged entity, and at most CONNS requests' worth more.
    #
    if [ "${stored:-0}" -lt $((n * per)) ] || [ "$stored" -gt $(((n + CONNS) * per)) ]; then
      die "wrk saw $n successful requests ($((n * per)) entities) but the store holds ${stored:-0}"
    fi
    runs+="$r"$'\n'
    stopBroker
  done
  printf '%s' "$runs" | median
}



# -----------------------------------------------------------------------------
#
# 7. Run, and report
#
say ""
say "Measuring $DIGEST"
say "Broker cores: $CORES (physical), load generator on CPUs $WRK_CPUS, wrk -t$WRK_THREADS -c$CONNS, ${DURATION}s x $REPEATS, median"
say ""

RESULTS=""
for n in $CORES; do
  cpus=$(brokerCpus "$n")
  say "== broker on $n core(s): CPU $cpus"

  say "   POST /entities ..."
  read -r rps p99 _ <<< "$(writeScenario "$cpus" "$WORK/create.lua" 1)"
  RESULTS+=$(printf '| POST /entities | %d | %d | %d | %d |' "$n" "$rps" "$rps" "$p99")$'\n'

  say "   POST /entityOperations/create (20 per request) ..."
  read -r rps p99 _ <<< "$(writeScenario "$cpus" "$WORK/batch.lua" 20)"
  RESULTS+=$(printf '| batch create, 20 per request | %d | %d | %d | %d |' "$n" "$rps" "$((rps * 20))" "$p99")$'\n'

  say "   GET /entities/{id} ..."
  read -r rps p99 _ <<< "$(readScenario "$cpus" "$BASE/entities/urn:ngsi-ld:Vehicle:1")"
  RESULTS+=$(printf '| GET /entities/{id} | %d | %d | - | %d |' "$n" "$rps" "$p99")$'\n'

  say "   GET /entities?type=Vehicle&limit=20 ..."
  read -r rps p99 _ <<< "$(readScenario "$cpus" "$BASE/entities?type=Vehicle&limit=20")"
  RESULTS+=$(printf '| GET /entities?type=Vehicle&limit=20 | %d | %d | - | %d |' "$n" "$rps" "$p99")$'\n'
done

cat <<EOF

## coraine throughput - reproduced $(date -u +%Y-%m-%dT%H:%MZ)

| | |
|---|---|
| Image | $DIGEST |
| CPU | $(awk -F: '/model name/ {sub(/^ /, "", $2); print $2; exit}' /proc/cpuinfo) |
| Kernel | $(uname -r) |
| Store | corDB - in memory, no persistence |
| Method | broker pinned to N physical cores; wrk -t$WRK_THREADS -c$CONNS on other cores; median of $REPEATS x ${DURATION}s; every response 2xx; writes verified against the store |

| Scenario | Cores | Requests/s | Entities/s | p99 (us) |
|---|---:|---:|---:|---:|
${RESULTS}
EOF
