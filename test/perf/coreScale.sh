#!/usr/bin/env bash
#
# coreScale.sh - does throughput follow the core count?
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# The throughput table in the README answers "what happens when clients pile
# onto fixed hardware". This answers the other one: give the broker more cores,
# does it do proportionally more work?
#
# Method: pin the broker to n PHYSICAL cores, pin the load generator to cores
# the broker is not using, sweep n. Report requests/s per core count.
#
# ⚠️ SMT is the trap, and it is not a hypothetical - the first run of this
# experiment put wrk on the SIBLINGS of the broker's own cores. Broker and load
# generator were then fighting over the same physical silicon, and the curve
# turned DOWN at the top, which reads exactly like a broker that fails to scale.
# It was the measurement eating itself. So the CPU list is read from
# /sys/devices/system/cpu rather than assumed: one logical CPU per physical
# core for the broker, and for wrk only cores the broker was never given.
#
# ⚠️ wrk runs on this same machine. It competes for cache and memory bandwidth,
# so the broker is if anything understated, and the sweep cannot reach the full
# core count - something has to drive the load. For numbers beyond half your
# cores, run the load generator on another machine.
#
# corDB by design, not by convenience. With mongoc, mongod runs on the same
# machine and takes cores of its own, so the curve would describe A BROKER AND A
# DATABASE SHARING ONE HOST and would bend where MongoDB stopped scaling rather
# than where the broker did. Both are real questions; this script answers "does
# the broker use the cores it is given", which means taking the storage engine
# out of the answer and leaving parsing, matching, rendering and HTTP.
#
# Usage:  test/perf/coreScale.sh [maxCores]
#
set -euo pipefail

PORT=${PORT:-1039}
DURATION=${DURATION:-5s}
REPEATS=${REPEATS:-3}
export SEAMWARE_PLUGIN_DIR=${SEAMWARE_PLUGIN_DIR:-/opt/seamware/plugins}

command -v wrk     >/dev/null || { echo "coreScale.sh: wrk is not installed" >&2; exit 1; }
command -v taskset >/dev/null || { echo "coreScale.sh: taskset is not installed" >&2; exit 1; }
command -v coraine >/dev/null || { echo "coreScale.sh: coraine is not on PATH" >&2; exit 1; }

#
# One logical CPU per physical core - the FIRST sibling of each core. Anything
# else and the sweep measures hyperthreads rather than cores.
#
mapfile -t cpus < <(
  for d in /sys/devices/system/cpu/cpu[0-9]*; do
    id=${d##*/cpu}
    sib=$(cut -d, -f1 < "$d/topology/thread_siblings_list" 2>/dev/null || echo "$id")
    [ "$sib" = "$id" ] && echo "$id"
  done | sort -n
)
physical=${#cpus[@]}
[ "$physical" -ge 4 ] || { echo "coreScale.sh: needs at least 4 physical cores, found $physical" >&2; exit 1; }

# Half the cores drive, half serve - the most the broker can get and still be measured.
maxCores=${1:-$(( physical / 2 ))}

loadCpus=$(IFS=,; echo "${cpus[*]:physical/2}")
echo ">>> $physical physical cores; broker up to $maxCores, load generator on $loadCpus"
echo

pkill -x coraine 2>/dev/null || true
sleep 1

n=1
while [ "$n" -le "$maxCores" ]; do
  brokerCpus=$(IFS=,; echo "${cpus[*]:0:n}")

  taskset -c "$brokerCpus" coraine --port "$PORT" --database corDB --troe none \
          --connectionPoolSize 32 > /tmp/coreScale.log 2>&1 &
  pid=$!
  until (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; do
    kill -0 $pid 2>/dev/null || { echo "broker died at n=$n"; cat /tmp/coreScale.log; exit 1; }
    sleep 0.2
  done

  # Same fixture as perfRun.sh: 100 five-attribute entities, ~550 B each.
  for i in $(seq 1 100); do
    curl -s -o /dev/null -X POST "http://localhost:$PORT/ngsi-ld/v1/entities" \
      -H 'Content-Type: application/json' \
      -d "{\"id\":\"urn:ngsi-ld:Vehicle:$i\",\"type\":\"Vehicle\",
           \"brandName\":{\"type\":\"Property\",\"value\":\"Mercedes-$i\"},
           \"speed\":{\"type\":\"Property\",\"value\":$i},
           \"colour\":{\"type\":\"Property\",\"value\":\"red\"},
           \"owner\":{\"type\":\"Relationship\",\"object\":\"urn:ngsi-ld:Person:$i\"},
           \"location\":{\"type\":\"GeoProperty\",\"value\":{\"type\":\"Point\",\"coordinates\":[$i,$i]}}}"
  done

  url="http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=20"
  taskset -c "$loadCpus" wrk -t8 -c50 -d2s "$url" > /dev/null 2>&1      # warm up
  best=0
  for _ in $(seq 1 "$REPEATS"); do
    rps=$(taskset -c "$loadCpus" wrk -t8 -c50 -d"$DURATION" "$url" 2>/dev/null |
          awk '/Requests\/sec/{printf "%.0f", $2}')
    [ "${rps:-0}" -gt "$best" ] && best=$rps
  done
  printf "%3d core(s)  %8d req/s\n" "$n" "$best"

  kill -TERM $pid 2>/dev/null || true
  wait $pid 2>/dev/null || true

  n=$(( n * 2 ))
done
