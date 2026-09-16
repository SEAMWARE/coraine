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
#
# Extra broker arguments. The reason this exists: the broker's own defaults move
# (--httpLoops became core-dependent), and a measurement that inherits a default
# is a measurement of whatever the default was that week. A documented number
# names its flags.
#
BROKER_ARGS=${PERF_BROKER_ARGS:-}
#
# PERF_BROKER_CORES - pin the broker to this many PHYSICAL cores and the load
# generator to cores the broker was never given. Unset: no pinning at all, which
# is what the nightly wants (it compares runs of itself on the same runner).
#
# Set it to 1 and the run answers "per core", which is the number worth
# publishing: a throughput figure without a core count beside it says more about
# the machine than about the broker.
#
# SMT is the trap, and not a hypothetical - it cost a whole afternoon in
# coreScale.sh, where the load generator landed on the SIBLINGS of the broker's
# own cores and the scaling curve turned DOWN at the top, which reads exactly
# like a broker that fails to scale. So one logical CPU per physical core, read
# from sysfs rather than assumed.
#
BROKER_CORES=${PERF_BROKER_CORES:-}
brokerPin=() ; loadPin=()
if [ -n "$BROKER_CORES" ]; then
  mapfile -t physCpus < <(
    for d in /sys/devices/system/cpu/cpu[0-9]*; do
      id=${d##*/cpu}
      sib=$(cut -d, -f1 < "$d/topology/thread_siblings_list" 2>/dev/null || echo "$id")
      [ "$sib" = "$id" ] && echo "$id"
    done | sort -n
  )
  physical=${#physCpus[@]}
  if [ "$BROKER_CORES" -ge "$physical" ]; then
    echo "perfRun.sh: PERF_BROKER_CORES=$BROKER_CORES leaves nothing to drive the load ($physical physical cores)" >&2
    exit 1
  fi
  #
  # The load generator gets every physical core the broker did not get. It runs
  # on this same machine and competes for cache and memory bandwidth either way,
  # so the broker is if anything understated - never flattered.
  #
  brokerPin=( taskset -c "$(IFS=,; echo "${physCpus[*]:0:BROKER_CORES}")" )
  loadPin=(   taskset -c "$(IFS=,; echo "${physCpus[*]:BROKER_CORES}")" )
  echo "perfRun.sh: broker on ${brokerPin[2]}, load generator on ${loadPin[2]}" >&2
fi

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
  echo "perfRun.sh: port $PORT is still in use after 10s, held by:" >&2
  ss -ltnp "sport = :$PORT" 2>/dev/null | tail -n +2 >&2 || fuser -n tcp "$PORT" >&2 2>/dev/null || true
  return 1
}

stopBroker() {
  local i
  [ -n "${brokerPid:-}" ] || return 0
  #
  # `|| return 0` used to be here, so a broker that had already exited took the
  # whole teardown with it - including the wait for the port to come back, which
  # is the part the NEXT run depends on.
  #
  kill "$brokerPid" 2>/dev/null || true
  for i in $(seq 1 100); do
    kill -0 "$brokerPid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$brokerPid" 2>/dev/null || true
  awaitPortFree || true
}

startBroker() {
  #
  # A port that is not free is FATAL, and it took a stale broker to notice that
  # it had not been. The sequence, all of it silent:
  #
  #   - something was still listening on $PORT
  #   - this broker could not bind, and exited
  #   - the readiness probe below got its 200 from the STALE broker
  #   - the fixture got 409, because that broker already had these entities
  #   - and at exit, stopBroker killed a pid that was already gone, so the
  #     stale broker outlived the run to do it all again to the next one
  #
  # awaitPortFree said so on stderr and returned 1 the whole time. Nothing
  # looked. The old comment below was right about the danger and guarded the
  # wrong half of it: asking the process is no use if the process is dead and
  # somebody else answers.
  #
  awaitPortFree || exit 1

  "${brokerPin[@]}" coraine --port "$PORT" $dbArgs --troe none $BROKER_ARGS > /tmp/perf-broker.log 2>&1 &
  brokerPid=$!
  local i
  for i in $(seq 1 60); do
    #
    # Ask the process BEFORE the port. A readiness probe that only asks "does
    # something answer on $PORT" passes against a broker this script did not
    # start, and then every number below belongs to the wrong binary.
    #
    kill -0 "$brokerPid" 2>/dev/null \
      || { echo "perfRun.sh: the broker exited during startup" >&2; cat /tmp/perf-broker.log >&2; exit 1; }
    curl -sf "http://localhost:$PORT/ngsi-ld/v1/types" > /dev/null && return 0
    sleep 0.5
  done
  echo "perfRun.sh: broker never came up" >&2; cat /tmp/perf-broker.log >&2; exit 1
}

#
# The fixture: five-attribute entities of ~550 bytes, so a limit=20 query returns
# about 11 KB - a realistic page rather than a toy.
#
# Created in batches. One POST per entity is 100 000 sequential round trips for
# the large-store runs, which is minutes of setup per measurement and made the
# big stores something nobody would measure twice - which is how a linear
# retrieve survived in corDB for months. The entities are byte-for-byte the ones
# the loop created, so numbers stay comparable across the change; only the
# transport that puts them there is different.
#
FIXTURE_CHUNK=${PERF_FIXTURE_CHUNK:-500}

entityJson() {
  printf '{"id":"urn:ngsi-ld:Vehicle:%d","type":"Vehicle",
         "brand":{"type":"Property","value":"Mercedes"},
         "speed":{"type":"Property","value":%d,"observedAt":"2026-08-20T10:00:00Z"},
         "location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[13.4,52.5]}},
         "isParked":{"type":"Relationship","object":"urn:ngsi-ld:OffStreetParking:%d"},
         "description":{"type":"Property","value":"a five-attribute vehicle used for throughput measurement, padded to roughly five hundred bytes so the numbers mean something ------------------------------------------------"}}' \
         "$1" "$(( $1 % 120 ))" "$1"
}

fixture() {
  local i from to body first
  for (( from=1; from<=ENTITIES; from+=FIXTURE_CHUNK )); do
    to=$(( from + FIXTURE_CHUNK - 1 ))
    [ "$to" -gt "$ENTITIES" ] && to=$ENTITIES
    body="[" ; first=1
    for (( i=from; i<=to; i++ )); do
      [ "$first" = 1 ] && first=0 || body+=","
      body+=$(entityJson "$i")
    done
    body+="]"
    #
    # 201 with a body listing the ids, or 207 if any of them existed. Anything
    # else and the store is short of what the numbers below will claim it is -
    # and curl exits 0 on a 400, so the status has to be looked at.
    #
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
                "http://localhost:$PORT/ngsi-ld/v1/entityOperations/create" \
                -H 'Content-Type: application/json' --data-binary "$body")
    case "$code" in
      201|207) ;;
      *) echo "perfRun.sh: the fixture got HTTP $code creating entities $from-$to" >&2; exit 1 ;;
    esac
  done

  #
  # And then count them. A fixture that half worked is the worst case: every
  # number below is real, reproducible, and about a store nobody described.
  #
  local have
  have=$(curl -s -D - -o /dev/null "http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=0&count=true" \
         | awk 'BEGIN{IGNORECASE=1} /^NGSILD-Results-Count:/{gsub(/\r/,""); print $2}')
  if [ "${have:-0}" != "$ENTITIES" ]; then
    echo "perfRun.sh: the store holds ${have:-0} entities, not $ENTITIES" >&2
    exit 1
  fi
}

#
# Emptying the store, for the scenarios that fill it.
#
# corDB keeps its entities in RAM, so a restart is the whole of it. mongoc needs
# the database dropped, or the next repeat starts on top of what the last one
# made - which is exactly the drift being avoided.
#
dropMongo() {
  [ "$DB" = "mongoc" ] || return 0
  mongosh --quiet --host "$HOST" --eval 'db.getSiblingDB("corperf").dropDatabase()' > /dev/null 2>&1 \
    || { echo "perfRun.sh: could not drop the mongo database - a create scenario would measure a growing store" >&2; exit 1; }
}

resetStore() {
  stopBroker
  dropMongo
  startBroker
  fixture
}

trap stopBroker EXIT
dropMongo          # whatever a previous run left behind is not this run's store
startBroker
fixture

#
# wrk refuses -c below -t ("number of connections must be >= threads") and says
# so on stdout with exit status 0, so the awk below simply found nothing and the
# scenario printed as `"patch_c1":,` - invalid JSON, no error anywhere. Any
# scenario with fewer connections than threads needs the thread count brought
# down to meet it.
#
wrkThreads() {
  local conns="$1"
  [ "$conns" -lt "$THREADS" ] && echo "$conns" || echo "$THREADS"
}

#
# And the reason that bug was invisible: a scenario that produces nothing still
# prints, as an empty field in a JSON object nobody validates. Every number goes
# through here, so a missing one stops the run where it happened.
#
median() {
  local what="$1" rps; shift
  rps=$(printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
  case "$rps" in
    ''|*[!0-9]*) echo "perfRun.sh: no requests/s from: $what" >&2
                 echo "perfRun.sh: the run collected [$*] - re-run that wrk by hand to see why" >&2
                 return 1 ;;
  esac
  echo "$rps"
}

#
# ⚠️ wrk counts a 409 as a request.
#
# createEntity.lua shadowed the thread slot it was given, so all eight threads
# built the same entity ids and seven of every eight requests came back 409 -
# and the scenario reported 166 036 creates/s, faster than a PATCH, which is
# impossible and looked like a triumph. 316 644 of 351 124 responses were
# errors. Nothing in the run said so, because "Requests/sec" does not care what
# the answer was.
#
# So every wrk run goes through here, and a single non-2xx answer stops the run.
# No scenario measured by this script has a legitimate one: reads answer 200,
# a PATCH 204, a batch 201/204/207.
#
wrkRun() {                      # wrkRun <label> <wrk args...>  -> requests/s
  local label="$1"; shift
  local out
  out=$("$@" 2>&1) || true
  local bad
  bad=$(printf '%s' "$out" | awk '/Non-2xx or 3xx responses:/{print $NF}')
  if [ -n "$bad" ]; then
    echo "perfRun.sh: $bad requests were answered with an error in: $label" >&2
    echo "perfRun.sh: a rate measured over error responses is not a rate - fix the scenario" >&2
    printf '%s\n' "$out" | sed 's/^/    /' >&2
    exit 1
  fi
  printf '%s' "$out" | awk '/Requests\/sec/{printf "%.0f", $2}'
}

# median of REPEATS runs - a single wrk run on a shared runner is a rumour
measure() {
  local url="$1" conns="$2" rpsList=() t
  t=$(wrkThreads "$conns")
  wrkRun "warmup $url" "${loadPin[@]}" wrk -t"$t" -c"$conns" -d2s "$url" > /dev/null
  for _ in $(seq 1 "$REPEATS"); do
    rpsList+=( "$(wrkRun "-t$t -c$conns $url" "${loadPin[@]}" wrk -t"$t" -c"$conns" -d"$DURATION" "$url")" )
  done
  median "-t$t -c$conns $url" "${rpsList[@]}"
}


#
# measureScript - the same, for a request wrk cannot express as a URL
#
# Anything that is not a GET needs a Lua script, because wrk's command line has
# no way to say "PATCH, with this body".
#
measureScript() {
  local script="$1" conns="$2" rpsList=() t
  t=$(wrkThreads "$conns")
  PERF_ENTITIES="$ENTITIES" PERF_BATCH="${PERF_BATCH:-20}" wrkRun "warmup $(basename "$script")" "${loadPin[@]}" wrk -t"$t" -c"$conns" -d2s -s "$script" "http://localhost:$PORT" > /dev/null
  for _ in $(seq 1 "$REPEATS"); do
    rpsList+=( "$(PERF_ENTITIES="$ENTITIES" PERF_BATCH="${PERF_BATCH:-20}" wrkRun "-t$t -c$conns -s $(basename "$script")" "${loadPin[@]}" wrk -t"$t" -c"$conns" -d"$DURATION" -s "$script" "http://localhost:$PORT")" )
  done
  median "-t$t -c$conns -s $(basename "$script")" "${rpsList[@]}"
}

queryC50=$(measure "http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=20" 50)
queryC200=$(measure "http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=20" 200)
retrieve=$(measure  "http://localhost:$PORT/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:7" 50)

#
# THE PAGE SIZE, held at one concurrency so the three are comparable.
#
# query_c50 above is limit=20, and for months every throughput number quoted
# anywhere was that one - read by everybody as "requests per second" with the
# 20 left off. It is also 20 ENTITIES per second times twenty, which is a
# different and much larger claim, and neither number is useful without the
# other.
#
# So all three page sizes, and entities/s is rps x the page size:
#
#   limit=1     the worst case for us - all per-request cost, no amortisation
#   limit=20    a realistic page
#   limit=100   where per-entity serialisation dominates and the servers diverge
#
queryL1C50=$(measure   "http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=1" 50)
queryL100C50=$(measure "http://localhost:$PORT/ngsi-ld/v1/entities?type=Vehicle&limit=100" 50)

#
# WRITES. Until 2026-09-15 this script measured three request shapes and every
# one of them was a read - so half of what a context broker does was unmeasured,
# and it was the half that was broken: corDB had no locking at all and twenty
# concurrent PATCHes killed the broker. Nothing here would ever have noticed.
#
# patch_c50 is the characteristic write - a device reporting a new value for an
# entity that already exists - at the same concurrency as query_c50, so the two
# are directly comparable.
#
# patch_c1 is the same write with ONE connection. The pair is the point: a
# server that is fast alone and collapses in company says so in the ratio, and
# that is exactly the shape the corDB bug had (30 433 req/s at c1, dead at c20).
#
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
patchC50=$(measureScript "$SCRIPTDIR/patchAttr.lua" 50)
patchC1=$(measureScript  "$SCRIPTDIR/patchAttr.lua" 1)

#
# The same write, twenty at a time. Per ENTITY it should be far cheaper - one
# HTTP request, one URL-param parse, one @context resolution and one lock
# acquisition amortised over twenty instead of paid twenty times.
#
# batch20_c50 x 20 against patch_c50 is the ratio worth watching: it says what
# batching is actually worth, and a ratio near 1 would say the per-request
# overhead is not where the time goes.
#
batch20C50=$(PERF_BATCH=20 measureScript "$SCRIPTDIR/batchUpdate.lua" 50)

#
# CREATES. Everything above leaves the store the size it found it: a query reads,
# a patch and a batch update change entities that are already there. A create
# does not, and a five-second run at 40 000/s puts 200 000 entities in - so the
# second repeat would start from a store three times the size of the first, the
# third from five times, and the median of the three would be the median of
# three different experiments.
#
# So the store is emptied before every repeat. That costs a broker restart and a
# fixture rebuild per repeat, which is why the fixture goes in as batches now.
#
#
# ⚠️ measureGrowing does NOT print its result, and that is not a style choice.
#
# It restarts the broker, and $( ) is a SUBSHELL: a reset inside one sets
# brokerPid in the subshell, so the parent went on holding the pid of a broker
# four resets dead, the last broker each subshell started was orphaned with
# nobody to kill it, and the next run found the port taken by a broker that no
# longer had a parent. Twenty minutes went into that, with every symptom -
# "port still in use", a fixture answering 409 - pointing somewhere else.
#
# So the result comes back in MEASURED, and the broker stays the main shell's
# business. measure() and measureScript() may print, because they never touch it.
#
MEASURED=
measureGrowing() {
  local script="$1" conns="$2" rpsList=() t
  t=$(wrkThreads "$conns")
  resetStore
  PERF_ENTITIES="$ENTITIES" PERF_BATCH="${PERF_BATCH:-20}" wrkRun "warmup $(basename "$script")" "${loadPin[@]}" wrk -t"$t" -c"$conns" -d2s -s "$script" "http://localhost:$PORT" > /dev/null
  for _ in $(seq 1 "$REPEATS"); do
    resetStore
    rpsList+=( "$(PERF_ENTITIES="$ENTITIES" PERF_BATCH="${PERF_BATCH:-20}" wrkRun "-t$t -c$conns -s $(basename "$script")" "${loadPin[@]}" wrk -t"$t" -c"$conns" -d"$DURATION" -s "$script" "http://localhost:$PORT")" )
  done
  #
  # And once more afterwards, so whatever runs next sees the store this script
  # says it set up rather than the wreckage of a create benchmark.
  #
  resetStore
  MEASURED=$(median "-t$t -c$conns -s $(basename "$script") (store reset per repeat)" "${rpsList[@]}")
}

measureGrowing "$SCRIPTDIR/createEntity.lua" 50 ; createC50=$MEASURED
measureGrowing "$SCRIPTDIR/createEntity.lua" 1  ; createC1=$MEASURED
PERF_BATCH=20 measureGrowing "$SCRIPTDIR/batchCreate.lua" 50 ; batch20CreateC50=$MEASURED

printf '{"db":"%s","query_c50":%s,"query_c200":%s,"query_l1_c50":%s,"query_l100_c50":%s,"retrieve_c50":%s,"patch_c50":%s,"patch_c1":%s,"batch20_c50":%s,"create_c50":%s,"create_c1":%s,"batch20create_c50":%s}\n' \
       "$DB" "$queryC50" "$queryC200" "$queryL1C50" "$queryL100C50" "$retrieve" "$patchC50" "$patchC1" "$batch20C50" "$createC50" "$createC1" "$batch20CreateC50"
