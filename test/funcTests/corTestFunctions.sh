# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - repo-specific test functions for coraine
#
# ⭐ A test waits on, and asserts on, what the API says - NEVER on the broker's log. A trace line is
# there only when the trace levels include it AND the code was built with its traces; no test sets
# COR_TRACE_LEVELS, and the suite passes with -traceLevels "". The bridge* helpers below are what
# to wait on instead (doc/testing.md, "Functional tests never read the log").
#
export COR_BROKER="${COR_BROKER:-coraine}"        # broker from PATH (installed via make di)
export COR_IMPORT="${COR_IMPORT:-coraine-import}" # the migration importer, ditto (doc/migration.md)
export COR_DB_NAME="${COR_DB_NAME:-corTest}"
#
# Where MongoDB is. The port has always been overridable; the HOST was assumed to
# be this machine, which stops being true the moment the suite runs anywhere the
# database is a separate container - a CI job with a mongo service, for one. Both
# now default to the local instance and are overridable together.
#
COR_MONGO_HOST=${COR_MONGO_HOST:-localhost}
COR_MONGO_PORT=${COR_MONGO_PORT:-27017}
COR_TROE_HOST=${COR_TROE_HOST:-localhost}          # timescale/postgres host - see COR_MONGO_HOST
COR_TROE_PORT=${COR_TROE_PORT:-5432}               # timescale/postgres port
COR_TROE_USER=${COR_TROE_USER:-postgres}           # timescale/postgres user

# Plugins from their install site; the tools - corTestClient, corRequest - from corLibs/bin, beside
# corTest (corTools builds them, corLibs installs them): SCRIPT_HOME is where corTest runs from.
COR_PLUGIN_DIR="${COR_PLUGIN_DIR:-/opt/seamware/plugins}"
COR_REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"


# -----------------------------------------------------------------------------
#
# Role definitions: port, pidFile, dbPrefix
#
#   role      port   pidFile                  dbPrefix
#
# Roles: CB = main broker; CB2-5 = secondary main brokers (federation /
# replication); CP1-5 = brokers acting as context providers. Ports stay below
# 1036 (reserved for the parallel ETSI run — see ~/bin/gateAll).
COR_ROLES="
   CB        1026    /tmp/coraine_CB.pid      corTest
   CP1       1027    /tmp/coraine_CP1.pid     corTest_cp1
   CP2       1028    /tmp/coraine_CP2.pid     corTest_cp2
   CP3       1029    /tmp/coraine_CP3.pid     corTest_cp3
   CP4       1030    /tmp/coraine_CP4.pid     corTest_cp4
   CP5       1031    /tmp/coraine_CP5.pid     corTest_cp5
   CB2       1032    /tmp/coraine_CB2.pid     corTest_cb2
   CB3       1033    /tmp/coraine_CB3.pid     corTest_cb3
   CB4       1034    /tmp/coraine_CB4.pid     corTest_cb4
   CB5       1035    /tmp/coraine_CB5.pid     corTest_cb5
"

CB_PORT=1026
CP1_PORT=1027
CP2_PORT=1028
CP3_PORT=1029
CP4_PORT=1030
CP5_PORT=1031
CB2_PORT=1032
CB3_PORT=1033
CB4_PORT=1034
CB5_PORT=1035

# corRoleLookup - resolve role to port/pidFile/dbPrefix
# Sets: COR_ROLE_PORT, COR_ROLE_PID_FILE, COR_ROLE_DB_PREFIX
corRoleLookup() {
  local role="$1"
  local line

  line=$(echo "$COR_ROLES" | awk -v r="$role" '$1 == r { print $2, $3, $4 }')
  if [ -z "$line" ]; then
    echo "corRoleLookup: unknown role: $role"
    return 1
  fi

  COR_ROLE_PORT=$(echo "$line" | awk '{print $1}')
  COR_ROLE_PID_FILE=$(echo "$line" | awk '{print $2}')
  COR_ROLE_DB_PREFIX=$(echo "$line" | awk '{print $3}')
}


# -----------------------------------------------------------------------------
#
# corStoreArgs <role> [params...] - the store options of a role (the DB plugin, the TRoE plugin and
# their options) followed by the params - what coraineStart and coraineImport put on a command line,
# so an import goes exactly where the next coraineStart reads. Printed; 1 on an unknown store.
#
corStoreArgs() {
  local role="$1"
  shift
  local -a extraParams=("$@")
  local args=""

  # Current-state DB plugin
  case "$COR_DB_TYPE" in
    mongoc) args="$args --database $COR_PLUGIN_DIR/db/currentState/mongoc.so --dbName $COR_ROLE_DB_PREFIX --dbHost $COR_MONGO_HOST --dbPort $COR_MONGO_PORT" ;;
    corDB)  args="$args --database $COR_PLUGIN_DIR/db/currentState/corDB.so"
            #
            # Every corDB broker is persistent, each role in a directory of its own - as with MongoDB, a
            # (re)start KEEPS what is there and corDbDrop empties it: a test that stops and starts its
            # broker finds its data, on corDB as on mongoc. A test that names its own --dbDir keeps it.
            #
            if ! printf '%s\n' "${extraParams[@]}" | grep -qxE -- '-?-dbDir'; then
              args="$args --dbDir $(corDbDir "$role")"
            fi
            ;;
    ramDB)  args="$args --database $COR_PLUGIN_DIR/db/currentState/ramDB.so" ;;   # corDB in RAM only: no --dbDir, no history
    NONE)   ;;  # compiled-in default
    *)      echo "corStoreArgs: unknown -db type: $COR_DB_TYPE"; return 1 ;;
  esac

  #
  # TRoE store. -troeDb corDB: a test's "--troe timescale" runs as "--troe corDB" - the history
  # inside the corDB store, so the temporal tests written against timescale check corDB's history
  # with the same expectations. Needs -db corDB (--troe corDB is the corDB store's own) - checked
  # only where a rewrite happens: a test that starts on its own database (the ring tests: ramDB,
  # --troe ramDB) is left as it is.
  #
  case "$COR_TROE_DB_TYPE" in
    NONE|"") ;;  # compiled-in default or unset
    corDB)   local i
             for i in "${!extraParams[@]}"; do
               if [ "${extraParams[$i]}" == "timescale" ] && [ "$i" -gt 0 ] && [[ "${extraParams[$((i-1))]}" =~ ^-?-troe$ ]]; then
                 if [ "$COR_DB_TYPE" != "corDB" ]; then echo "corStoreArgs: -troeDb corDB needs -db corDB"; return 1; fi
                 extraParams[$i]="corDB"
               fi
             done
             ;;
    *)       echo "corStoreArgs: unknown -troeDb type: $COR_TROE_DB_TYPE"; return 1 ;;
  esac

  # Timescale TRoE convenience: when a test asks for "--troe timescale" without
  # naming the DB, derive the role-keyed name (corh_<role>) — the same name
  # corTroeInit/corTroeDrop create/drop — and add --troeUser. Tests that pass an
  # explicit --troeName keep full control.
  if printf '%s\n' "${extraParams[@]}" | grep -qx 'timescale' && \
     ! printf '%s\n' "${extraParams[@]}" | grep -qx -- '--troeName'; then
    extraParams+=(--troeName "$(corTroeDbName "$role")" --troeUser "$COR_TROE_USER" --troeHost "$COR_TROE_HOST" --troePort "$COR_TROE_PORT")
  fi

  if [ ${#extraParams[@]} -gt 0 ]; then
    args="$args ${extraParams[*]}"
  fi

  echo "$args"
}



# -----------------------------------------------------------------------------
#
# coraineImport [-role <role>] --file <stream> [params...] - coraine-import (doc/migration.md) into the
# stores of a role, as coraineStart would start the broker on them
#
# It runs in the foreground: its report (stderr - the refused records and the counts) is the test's
# output, its log (stdout) goes to the role's log file, and its exit code is the function's.
#
coraineImport() {
  local role="CB"
  local -a extraParams

  while [ $# -gt 0 ]; do
    if [ "$1" == "-role" ]; then role="$2"; shift
    else extraParams+=("$1")
    fi
    shift
  done

  corRoleLookup "$role" || return 1

  local cmd="$COR_IMPORT"
  [ -n "$COR_TRACE_LEVELS" ] && cmd="$cmd --traceLevels $COR_TRACE_LEVELS"

  local storeArgs
  storeArgs=$(corStoreArgs "$role" "${extraParams[@]}") || { echo "$storeArgs"; return 1; }

  $cmd $storeArgs 2>&1 > "/tmp/coraine-import.${role}.log"
}



# -----------------------------------------------------------------------------
#
# coraineStart [-role <role>] [extra-broker-params...]
#
# Usage:  coraineStart
#         coraineStart -role CP1
#         coraineStart -role CP1 -distOps
#
coraineStart() {
  local role="CB"
  local -a extraParams

  while [ $# -gt 0 ]; do
    if [ "$1" == "-role" ]; then role="$2"; shift
    else extraParams+=("$1")
    fi
    shift
  done

  corRoleLookup "$role" || return 1
  coraineStop -role "$role" 2>/dev/null

  # --httpEndpoint is pinned to localhost so served-@context URLs, distributed-sub
  # callbacks and forwarded Link headers are host-independent (the broker now
  # auto-detects a LAN IP by default, which would make expected outputs vary per
  # test machine). Tests that need a different endpoint append their own -he.
  local cmd="$COR_BROKER --port $COR_ROLE_PORT --pretty-print 2 --foreground --httpEndpoint http://localhost:$COR_ROLE_PORT"

  #
  # Traces go to the role's log file (never to the test's stdout, which is what
  # the expect is matched against). Default set in corTestParams.sh; -traceLevels ""
  # turns them off for a test that measures something.
  #
  [ -n "$COR_TRACE_LEVELS" ] && cmd="$cmd --traceLevels $COR_TRACE_LEVELS"

  #
  # The stores - the DB and TRoE plugins and their options - and the test's own parameters
  #
  local storeArgs
  storeArgs=$(corStoreArgs "$role" "${extraParams[@]}") || { echo "$storeArgs"; return 1; }
  cmd="$cmd $storeArgs"

  #
  # COR_TRANSPORT=cor: every broker also serves cor://, on its HTTP port + 1000 - where corCurl looks
  # for it. A test that opens the port itself (-corPort) is left alone.
  #
  local corPort=""
  if [ "$COR_TRANSPORT" == "cor" ] && ! printf '%s\n' "${extraParams[@]}" | grep -qxE -- '-?-corPort'; then
    corPort=$((COR_ROLE_PORT + 1000))
    cmd="$cmd --corPort $corPort"
  fi

  # Valgrind (--vt): only the main broker (CB) runs under valgrind — wrapping
  # every broker in a multi-broker test would interleave their reports and
  # destroy the CB result (and triple the wall-clock). Scope errors to
  # definite+indirect leaks: "possibly lost" (interior-pointer-only) and "still
  # reachable" (process-lifetime globals) are excluded, so we don't need an
  # atexit teardown to get a clean run. onSignal()->exit(0) already lets
  # valgrind emit its report on the graceful stop below.
  local awaitSecs=10
  if [ "$COR_VALGRIND" == "1" ] && [ "$role" == "CB" ]; then
    local vgLog="${COR_VALGRIND_LOG:-/tmp/corValgrind}"
    # errors-for-leak-kinds=none: leaks must NOT inflate "ERROR SUMMARY", so that
    # line stays a pure memory-error count (Invalid read/write, uninitialised, …).
    # The engine fails on leaks by reading the LEAK SUMMARY lost-byte counts
    # directly, so leaks don't need to count as "errors" to be caught — and this
    # keeps the engine's E (errors) and L (leaks) tallies cleanly separated.
    #
    # --track-origins is the expensive one - it roughly doubles memcheck's cost,
    # and on a shared CI runner that turns a graceful shutdown into a two-minute
    # wait. It stays ON here, where the origins are actually read while chasing a
    # leak, and CI sets COR_VALGRIND_ORIGINS=no: there, valgrind is an indicator,
    # not the investigation.
    #
    local vgOrigins=${COR_VALGRIND_ORIGINS:-yes}
    local vg="valgrind --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=none --track-origins=$vgOrigins --num-callers=40 --child-silent-after-fork=yes"
    if [ -f "test/funcTests/valgrind.supp" ]; then
      vg="$vg --suppressions=test/funcTests/valgrind.supp"
    fi
    vg="$vg --log-file=${vgLog}.%p.vg"
    cmd="$vg $cmd"
    awaitSecs=90   # valgrind makes startup ~20x slower
  fi

  $cmd > "/tmp/coraine.${role}.log" 2>&1 &
  echo $! > "$COR_ROLE_PID_FILE"
  corAwaitPort $COR_ROLE_PORT $awaitSecs

  # cor:// opens just after HTTP: HTTP-ready is not cor-ready.
  # An if, not '[ ] &&': as the function's last command, a false test would be its return code
  if [ -n "$corPort" ]; then
    corAwaitPort $corPort $awaitSecs
  fi
}


# -----------------------------------------------------------------------------
#
# corValgrindReportComplete - has valgrind finished writing THIS test's report?
#
# The .vg is what the verdict is read from, so it is what a graceful stop has to
# wait for. Every log of this test must carry both summaries; no log at all is
# "not yet", so a caller that polls this gives up on its own bound rather than on
# the first look.
#
corValgrindReportComplete() {
  local base="${COR_VALGRIND_LOG:-/tmp/corValgrind}"
  local f found=0

  for f in "$base".*.vg; do
    [ -e "$f" ] || continue
    found=1
    grep -q "HEAP SUMMARY"  "$f" || return 1
    grep -q "ERROR SUMMARY" "$f" || return 1
  done

  [ $found == 1 ]
}


# -----------------------------------------------------------------------------
#
# coraineStop [-role <role>] [-all]
#
# No argument stops the CB, as it always has. -all stops every role in COR_ROLES,
# for teardowns that would otherwise have to name each secondary they started -
# forgetting one leaks a broker onto its port, which is the orphan flakiness in
# corTestFunctions' own stop helpers.
#
coraineStop() {
  local role="CB"
  local all=0

  while [ $# -gt 0 ]; do
    if   [ "$1" == "-role" ]; then role="$2"; shift
    elif [ "$1" == "-all" ];  then all=1
    fi
    shift
  done

  if [ $all == 1 ]; then
    local r
    for r in $(echo "$COR_ROLES" | awk '{print $1}'); do
      coraineStop -role "$r"
    done
    return 0
  fi

  corRoleLookup "$role" || return 1

  # Under valgrind (--vt), the CB must be stopped GRACEFULLY: SIGTERM lets
  # onSignal()->exit(0) run, which is what makes valgrind write its leak
  # report. A quick SIGKILL would truncate it. Wait (bounded) for the valgrind
  # process to actually exit before returning.
  #
  # Two things about that wait, both learned from a nightly that scored
  # "valgrind: report INCOMPLETE - broker killed before finish" on a test that
  # had otherwise passed:
  #
  #   - corPidAlive reads /proc/<pid>/stat, and that is the state of the THREAD
  #     GROUP LEADER. A leader that has exited while sibling threads keep running
  #     reports Z - which is exactly the shape of valgrind's shutdown, where the
  #     guest's exit is taken on whichever thread handled the signal and the
  #     final leak check runs on after it. "Z" there means the report is being
  #     written, not that it has been.
  #   - the SIGKILL below was never conditional, whatever its comment said. It
  #     fired on every stop, and in that window it is not a backstop against a
  #     hung broker: it is the thing that truncates the report.
  #
  # So: wait for the pid, then wait for the REPORT - the observable state the
  # verdict is actually read from - and only reach for SIGKILL if the graceful
  # wait genuinely ran out. The report wait is bounded and never fatal: a broker
  # that CRASHED leaves a report that will never gain an ERROR SUMMARY, and that
  # is a result to report rather than a reason to stand here. When all is well
  # the file is already complete, and this costs one grep.
  #
  if [ "$COR_VALGRIND" == "1" ] && [ "$role" == "CB" ] && [ -f "$COR_ROLE_PID_FILE" ]; then
    local pid; pid=$(cat "$COR_ROLE_PID_FILE")
    if corPidAlive "$pid"; then
      kill -TERM "$pid" 2>/dev/null
      local n=0
      while corPidAlive "$pid" && [ $n -lt 1200 ]; do sleep 0.1; n=$((n + 1)); done

      local m=0
      while ! corValgrindReportComplete && [ $m -lt 100 ]; do sleep 0.1; m=$((m + 1)); done

      if [ $n -ge 1200 ]; then
        kill -9 "$pid" 2>/dev/null   # the graceful wait ran out - now it is a backstop
      fi
    fi
    \rm -f "$COR_ROLE_PID_FILE"
    return
  fi

  # The broker this helper started: by its pid file - SIGTERM, then watched
  # 10 ms at a time with corPidAlive (no fork). A stop used to cost ~1.2 s here:
  # the broker took ~1 s to exit (the periodic loop's one-second sleep), and
  # each check of the wait loop below was a pgrep -f (~30 ms) plus a 0.1 s sleep.
  local pat="coraine.*--port $COR_ROLE_PORT( |\$)"
  local pid=""
  [ -f "$COR_ROLE_PID_FILE" ] && pid=$(< "$COR_ROLE_PID_FILE")

  if [ -n "$pid" ] && [ -r "/proc/$pid/cmdline" ] && command grep -q coraine "/proc/$pid/cmdline" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null                # SIGTERM → onSignal()->dbClose()->exit(0)
    local w=0
    while corPidAlive "$pid" && [ $w -lt 500 ]; do sleep 0.01; w=$((w + 1)); done
    if ! corPidAlive "$pid"; then
      \rm -f "$COR_ROLE_PID_FILE"
      return
    fi
  fi

  # Port-based kill so orphans from aborted prior runs (with no live pid
  # file) are still caught. Matches any coraine whose cmdline carries
  # "--port <port>".
  pkill -f "$pat" 2>/dev/null                    # SIGTERM → onSignal()->dbClose()->exit(0)

  # Wait (bounded) for graceful exit before the SIGKILL backstop. The SIGTERM
  # shutdown runs dbClose() (frees the store / closes mongo), which can take
  # longer than a fixed 0.1s — a premature SIGKILL prints "Killed" to the
  # launching shell's stderr and trips the stderr-empty gate (seen on the
  # mongoc persist/restart tests). 5s is ample; the backstop still catches a
  # hung broker.
  local n=0
  while pgrep -f "$pat" >/dev/null 2>&1 && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
  pkill -9 -f "$pat" 2>/dev/null                 # backstop only if still alive

  \rm -f "$COR_ROLE_PID_FILE"
}


# -----------------------------------------------------------------------------
#
# corDbDrop [-role <role>] [-tenant <tenant>] [-db <dbName>]
#
# Usage:  corDbDrop                    # drop collections in CB's default db
#         corDbDrop -tenant t1         # drop collections in CB's tenant db
#         corDbDrop -role CP1          # drop collections in CP1's db
#         corDbDrop -db coraine       # drop the entire "coraine" database
#
# -----------------------------------------------------------------------------
#
# COR_MONGO_DROP - corTools' corMongoDrop, beside corTest in corLibs/bin: one libmongoc connection,
# ~8 ms a drop. mongosh - a JavaScript runtime, ~0.3 s a drop, before almost every test - only where
# corMongoDrop was not built (no libmongoc there).
#
COR_MONGO_DROP=${SCRIPT_HOME:-$COR_REPO_DIR/../corLibs/bin}/corMongoDrop

corDbDrop() {
  local role="CB"
  local tenant=""
  local explicitDb=""

  while [ $# -gt 0 ]; do
    if   [ "$1" == "-role" ];   then role="$2"; shift
    elif [ "$1" == "-tenant" ]; then tenant="$2"; shift
    elif [ "$1" == "-db" ];     then explicitDb="$2"; shift
    fi
    shift
  done

  case "$COR_DB_TYPE" in
    mongoc)
      if [ -x "$COR_MONGO_DROP" ]; then
        if [ -n "$explicitDb" ]; then
          "$COR_MONGO_DROP" --host $COR_MONGO_HOST --port $COR_MONGO_PORT --db "$explicitDb" > /dev/null 2>&1
        else
          corRoleLookup "$role" || return 1
          if [ -n "$tenant" ]; then
            "$COR_MONGO_DROP" --host $COR_MONGO_HOST --port $COR_MONGO_PORT --db "${COR_ROLE_DB_PREFIX}-${tenant}" --collections entities,subscriptions,registrations,snapshots > /dev/null 2>&1
          else
            "$COR_MONGO_DROP" --host $COR_MONGO_HOST --port $COR_MONGO_PORT --prefix "$COR_ROLE_DB_PREFIX" > /dev/null 2>&1
          fi
        fi
      elif [ -n "$explicitDb" ]; then
        mongosh --host $COR_MONGO_HOST --port $COR_MONGO_PORT --quiet --eval 'db.dropDatabase()' "$explicitDb" > /dev/null 2>&1
      else
        corRoleLookup "$role" || return 1
        local db="$COR_ROLE_DB_PREFIX"
        if [ -n "$tenant" ]; then
          db="${db}-${tenant}"
          mongosh --host $COR_MONGO_HOST --port $COR_MONGO_PORT --quiet --eval 'db.entities.drop(); db.subscriptions.drop(); db.registrations.drop(); db.snapshots.drop()' "$db" > /dev/null 2>&1
        else
          # No tenant specified → drop default + all tenant-suffixed dbs.
          # Tests that leave tenant state behind shouldn't bleed into later
          # tests that assume ngsild_tenants_total == 1.
          local prefix="$COR_ROLE_DB_PREFIX"
          mongosh --host $COR_MONGO_HOST --port $COR_MONGO_PORT --quiet --eval \
            "db.adminCommand('listDatabases').databases \
              .map(d=>d.name) \
              .filter(n=>n===\"$prefix\"||n.startsWith(\"$prefix-\")) \
              .forEach(n=>db.getSiblingDB(n).dropDatabase())" > /dev/null 2>&1
        fi
      fi
      ;;
    corDB)
      #
      # The role's directory (coraineStart) - every tenant; -tenant: that tenant's; -db coraine: the
      # broker's own data (the @contexts) lives in the default tenant, so the role's directory with it
      #
      local dir
      dir=$(corDbDir "$role")
      if [ -n "$tenant" ]; then
        rm -rf "$dir/$(corDbTenantDirName "$tenant")"
      else
        rm -rf "$dir"
      fi
      ;;
    NONE)
      # No-op: broker restart clears the RAM store
      ;;
  esac
}



# -----------------------------------------------------------------------------
#
# corDbDir <role> - a corDB broker's --dbDir in the functests: ${COR_DB_PERSIST_DIR:-/tmp/corTest-dbDir}/<role>
#
corDbDir() {
  echo "${COR_DB_PERSIST_DIR:-/tmp/corTest-dbDir}/$1"
}



# -----------------------------------------------------------------------------
#
# corDbTenantDirName <tenant> - the name corDB gives a tenant's directory: every byte outside [A-Za-z0-9-]
# written %XX (corDB's corDbPersist.c, tenantDir)
#
corDbTenantDirName() {
  local name="$1" out="" c i
  for (( i=0; i<${#name}; i++ )); do
    c="${name:$i:1}"
    if [[ "$c" =~ [A-Za-z0-9-] ]]; then out+="$c"; else out+=$(printf '%%%02X' "'$c"); fi
  done
  echo "$out"
}

# corDbInit: drop + recreate
corDbInit() {
  corDbDrop "$@"
}


# -----------------------------------------------------------------------------
#
# TRoE (timescale/postgres) database helpers — the postgres counterpart of
# corDbDrop/corDbInit. Role-keyed like the mongo helpers: the TRoE DB for a role
# is "corh_<role>" (lowercased), so CB -> corh_cb, CP1 -> corh_cp1.
# coraineStart derives the same name for "--troe timescale".
#
#   corTroeDbName [role]            # echo the derived DB name (default CB)
#   corTroeInit  [-role R] [-db N]  # DROP + CREATE the TRoE DB
#   corTroeDrop  [-role R] [-db N]  # DROP the TRoE DB (and its snapshot children)
#
corTroeDbName() {
  local role="${1:-CB}"
  echo "corh_${role,,}"
}

corTroeInit() {
  local role="CB" db=""
  while [ $# -gt 0 ]; do
    if   [ "$1" == "-role" ]; then role="$2"; shift
    elif [ "$1" == "-db" ];   then db="$2";   shift
    fi
    shift
  done
  [ -z "$db" ] && db="$(corTroeDbName "$role")"

  # Drop any per-tenant / per-snapshot child databases ("<db>_<suffix>") left
  # by a previous run before recreating the base — each tenant now owns its own
  # physical database, so stale children would otherwise leak across runs.
  psql -h "$COR_TROE_HOST" -p "$COR_TROE_PORT" -U "$COR_TROE_USER" -tAc \
    "SELECT datname FROM pg_database WHERE datname LIKE '${db}_%'" 2>/dev/null | \
    while read -r child; do
      [ -n "$child" ] && psql -h "$COR_TROE_HOST" -p "$COR_TROE_PORT" -U "$COR_TROE_USER" -c "DROP DATABASE IF EXISTS \"$child\"" >/dev/null 2>&1
    done

  psql -h "$COR_TROE_HOST" -p "$COR_TROE_PORT" -U "$COR_TROE_USER" -c "DROP DATABASE IF EXISTS $db" >/dev/null 2>&1
  psql -h "$COR_TROE_HOST" -p "$COR_TROE_PORT" -U "$COR_TROE_USER" -c "CREATE DATABASE $db"          >/dev/null
}

corTroeDrop() {
  local role="CB" db=""
  while [ $# -gt 0 ]; do
    if   [ "$1" == "-role" ]; then role="$2"; shift
    elif [ "$1" == "-db" ];   then db="$2";   shift
    fi
    shift
  done
  [ -z "$db" ] && db="$(corTroeDbName "$role")"

  # Drop per-tenant / per-snapshot child TRoE DBs first ("<db>_<suffix>", e.g.
  # "<db>_t1" or "<db>_snap_<hex>"), then the base. Each tenant now owns its own
  # physical database; a child with the base as a prefix would otherwise leak
  # across runs.
  psql -h "$COR_TROE_HOST" -p "$COR_TROE_PORT" -U "$COR_TROE_USER" -tAc \
    "SELECT datname FROM pg_database WHERE datname LIKE '${db}_%'" 2>/dev/null | \
    while read -r child; do
      [ -n "$child" ] && psql -h "$COR_TROE_HOST" -p "$COR_TROE_PORT" -U "$COR_TROE_USER" -c "DROP DATABASE IF EXISTS \"$child\"" >/dev/null 2>&1
    done
  psql -h "$COR_TROE_HOST" -p "$COR_TROE_PORT" -U "$COR_TROE_USER" -c "DROP DATABASE IF EXISTS $db" >/dev/null 2>&1
}

# Default-role (CB) TRoE DB name, for tests that inspect the TRoE tables
# directly with `psql -d "$COR_TROE_DB"`.
export COR_TROE_DB="$(corTroeDbName CB)"


# -----------------------------------------------------------------------------
#
# corSnapDrop [-role <role>]
#
# Drop every snapshot-tenant DB belonging to <role> (default: CB). Snap
# tenants are named "${prefix}-${role}-_snap_<hex>" by snapshotTenantCreate;
# this enumerates them via listDatabases and dropDatabase()s each.
#
# corDbDrop already enumerates ${prefix}-* (so it incidentally cleans
# snap-tenants too), but corSnapDrop is the explicit, surgical helper for
# tests that want to assert "snapshots cleaned, nothing else touched".
# Recommended: call from snapshot tests' INIT (clean leftovers) and
# TEARDOWN (clean what this test created).
#
corSnapDrop() {
  local role="CB"

  while [ $# -gt 0 ]; do
    if [ "$1" == "-role" ]; then role="$2"; shift; fi
    shift
  done

  case "$COR_DB_TYPE" in
    mongoc)
      corRoleLookup "$role" || return 1
      local rolePrefix="${COR_ROLE_DB_PREFIX}-"
      if [ -x "$COR_MONGO_DROP" ]; then
        "$COR_MONGO_DROP" --host $COR_MONGO_HOST --port $COR_MONGO_PORT --prefix "$COR_ROLE_DB_PREFIX" --contains -_snap_ > /dev/null 2>&1
        return
      fi
      mongosh --host $COR_MONGO_HOST --port $COR_MONGO_PORT --quiet --eval \
        "db.adminCommand('listDatabases').databases \
          .map(d=>d.name) \
          .filter(n=>n.startsWith(\"$rolePrefix\")&&n.includes(\"-_snap_\")) \
          .forEach(n=>db.getSiblingDB(n).dropDatabase())" > /dev/null 2>&1
      ;;
    corDB)
      # A snapshot's tenant is "<tenant>-_snap_<hex>"; corDB writes the '_' of a tenant's directory as %5F
      rm -rf "$(corDbDir "$role")"/*%5Fsnap%5F*
      ;;
    NONE)
      ;;
  esac
}


# -----------------------------------------------------------------------------
#
# corTestClient - generic mock endpoint for forward-target / notification-receiver tests
#
# Each instance has its own PID file keyed by port, so multiple ftClients
# can run concurrently (one per CSR target) and be stopped individually.
#
COR_TEST_CLIENT=${SCRIPT_HOME:-$COR_REPO_DIR/../corLibs/bin}/corTestClient
FT_CLIENT_PORT=7701                          # default port when none given



# -----------------------------------------------------------------------------
#
# corRequest <options> - one cor:// request, printed like curl -i (STATUS, headers, empty line, body)
#
# The test tool of the same name; stdin closed, so it never waits on the test's own input.
#   corRequest --url cor://localhost:$((CB_PORT + 1000)) --path /ngsi-ld/v1/entities/urn:E1
#
COR_REQUEST=${SCRIPT_HOME:-$COR_REPO_DIR/../corLibs/bin}/corRequest

corRequest() {
  $COR_REQUEST "$@" < /dev/null
}


# -----------------------------------------------------------------------------
#
# corPortWait <port> - until a cor:// listener accepts on <port> (5 s at most)
#
# The broker opens it just AFTER its HTTP port - it needs the services HTTP registers - so a broker
# that answers HTTP may not be listening for cor:// yet.
#
corPortWait() {
  for i in $(seq 1 50); do corPortOpen "$1" && return 0; sleep 0.1; done
  echo "corPortWait: nothing listening on $1" >&2
  return 1
}


# ftClientStart [--port P] [--status S] [...extra]
#
# Starts a corTestClient on the given port (default 7701). --status sets the
# HTTP status returned for incoming POSTs (default 201). Pass "misbehave"
# statuses (503, 500, 403, ...) to simulate forwarding-target failures.
#
ftClientStart() {
  local port=$FT_CLIENT_PORT
  local -a extraParams

  while [ $# -gt 0 ]; do
    if [ "$1" == "--port" ] || [ "$1" == "-p" ]; then
      port="$2"
      FT_CLIENT_PORT="$port"
      shift
    else
      extraParams+=("$1")
    fi
    shift
  done

  local pidFile=/tmp/corTestClient.$port.pid
  ftClientStop --port $port 2>/dev/null

  # Record the scheme so ftClientDump/ftClientCount reach the right URL: a
  # --httpsKey/--httpsCertificate corTestClient serves HTTPS, plain HTTP otherwise.
  local scheme=http
  case " ${extraParams[*]} " in
    *" --httpsKey "*|*" -k "*) scheme=https ;;
  esac
  echo "$scheme" > /tmp/corTestClient.$port.scheme

  #
  # Keep what it says. When a corTestClient cannot start - no TLS in the HTTP library,
  # a port already taken, a missing certificate - the only symptom used to be
  # "corAwaitPort: port N not ready", because its stderr went to /dev/null. The
  # reason was always one line long and always discarded.
  #
  $COR_TEST_CLIENT --port $port ${extraParams[*]} > /tmp/corTestClient.$port.log 2>&1 &
  echo $! > "$pidFile"

  if ! corAwaitPort $port 5; then
    echo "ftClientStart: nothing listening on $port; corTestClient said:" >&2
    head -5 /tmp/corTestClient.$port.log >&2
    return 1
  fi

  #
  # The HTTP port being up says nothing about the MQTT subscription, which is a
  # separate thread: it retries the connect, and subscribes from the on-connect
  # callback. Return here and a test proceeds to trigger a notification that the
  # broker publishes to a topic with NO SUBSCRIBER YET - and an MQTT publish with
  # no subscriber is DISCARDED, not queued. The notification is not late, it is
  # gone, and every count in the test reads zero.
  #
  # That is a real nightly failure (subscription_notify_mqtt_qos_version,
  # 2026-08-26), and no amount of sleeping at the ASSERT end can recover it,
  # because the loss already happened at the start.
  #
  # So: wait for the SUBACK. corTestClient answers 1 on /mqttReady when it has one,
  # and 1 immediately when no --mqttPort was given, so this costs a single poll
  # in the common case.
  #
  case " ${extraParams[*]} " in
    *" --mqttPort "*)
      #
      # 8s, which is longer than it looks: the common case returns on the FIRST
      # poll, and this bound only applies when something is wrong. It has to
      # exceed corTestClient's own 5s connect deadline, or the barrier would give up
      # first and report a timeout over the top of the specific reason corTestClient
      # was about to publish.
      #
      local deadline=400                             # 400 x 0.02s = 8s
      [ "$COR_VALGRIND" == "1" ] && deadline=2000    # 40s under valgrind
      local i ready
      for ((i = 0; i < deadline; i++)); do
        ready=$(curl -sk "$(ftClientUrl $port /mqttReady)" 2>/dev/null)
        [ "$ready" == "1" ] && break
        [ "$ready" == "-1" ] && break                # given up on - waiting cannot help
        sleep 0.02
      done
      if [ "$ready" != "1" ]; then
        #
        # Say WHICH port and WHAT was answered. The first version of this message
        # named $port - the corTestClient's HTTP port - while the thing that had failed
        # was the MQTT broker on a different one, and then printed a corTestClient log
        # that was empty, so the report carried no information at all.
        #
        local mqttPort=""
        local n
        for ((n = 1; n <= ${#extraParams[@]}; n++)); do
          [ "${extraParams[n-1]}" == "--mqttPort" ] && mqttPort="${extraParams[n]}"
        done

        if [ "$ready" == "-1" ]; then
          echo "ftClientStart: corTestClient gave up on the MQTT broker" >&2
        else
          echo "ftClientStart: no MQTT SUBACK within $((deadline / 50))s" >&2
        fi
        echo "  corTestClient HTTP port : $port" >&2
        echo "  MQTT broker port   : ${mqttPort:-unknown}" >&2
        echo "  /mqttReady said    : '${ready}'" >&2
        if [ -n "$mqttPort" ] && ! corPortOpen "$mqttPort"; then
          echo "  -> NOTHING is listening on $mqttPort - the MQTT broker never came up," >&2
          echo "     so no deadline here could have helped. Check for a port collision." >&2
        fi
        if [ -s /tmp/corTestClient.$port.log ]; then
          echo "  corTestClient log:" >&2
          head -5 /tmp/corTestClient.$port.log >&2
        else
          echo "  corTestClient log /tmp/corTestClient.$port.log is empty or absent" >&2
        fi
        return 1
      fi
      ;;
  esac
}


# corPortOpen <port> - true when something accepts a TCP connection on <port>
#
# The probe runs in a CHILD BASH, deliberately. Written inline as
# `(exec 3<>/dev/tcp/127.0.0.1/$port)` it behaves differently depending on how it
# is embedded: on its own in an `if` it is fine, but under `cond && ! (...)` bash
# skips the subshell fork and the successful redirection takes the CALLING shell
# down with it - silently, mid-function, on the SUCCESS path only. That is how it
# hides: the failure path forks normally and behaves. One fork per poll costs
# nothing here, and does the same thing in every position.
#
corPortOpen() {
  bash -c "exec 3<>/dev/tcp/127.0.0.1/$1" >/dev/null 2>&1
}


# mosquittoWait <port> [seconds] - block until a mosquitto is listening on <port>
#
# `mosquitto -d` DAEMONISES, so its parent exits 0 before the listener is bound -
# a failed bind is reported by nothing at all: exit code 0, no stderr, no pid file.
# The tests used to follow it with `sleep 0.3`, which is not a check and cannot be
# one; when the bind actually failed the suite continued against a dead port, and
# the failure surfaced much later as an ftClientStart barrier timeout with an empty
# diagnostic (subscription_notify_mqtt_qos_version, Deploy, 2026-08-28).
#
# This is also why corTestClient's own connect loop cannot cover it: it retries forever,
# which is right when the broker is merely slow and useless when it is never coming.
#
# It proves SOMETHING accepts TCP on that port, not that it is this test's own
# mosquitto - a squatter would satisfy it. That gap is closed by giving every MQTT
# test its own port, which is the actual fix; this is the loud failure for when
# one is not there at all.
#
mosquittoWait() {
  local port=$1
  local secs=${2:-5}
  local i

  for ((i = 0; i < secs * 50; i++)); do
    corPortOpen "$port" && return 0
    sleep 0.02
  done

  echo "mosquittoWait: nothing listening on port $port after ${secs}s" >&2
  return 1
}


# mosquittoStop <conf> <port> - stop the test's mosquitto and wait until <port> is free
#
# pkill only SIGNALS: it returns while mosquitto is still shutting down, its port still bound. The next
# test to start a mosquitto on that port then failed to bind - silently, `mosquitto -d` daemonises -
# while mosquittoWait was satisfied by the old one still listening; the old one then exited and the
# test found nothing on its port (bridge_mqtt_v311_echo after bridge_mqtt, both on 11883).
#
mosquittoStop() {
  local conf=$1
  local port=$2
  local i

  pkill -f "mosquitto -d -c $conf" 2>/dev/null
  for ((i = 0; i < 250; i++)); do
    corPortOpen "$port" || return 0
    sleep 0.02
  done

  echo "mosquittoStop: port $port still open 5s after stopping the mosquitto of $conf" >&2
  return 1
}


# ftClientUrl <port> <path> - scheme-correct URL for the corTestClient on <port>
#
ftClientUrl() {
  local scheme=http
  [ -f "/tmp/corTestClient.$1.scheme" ] && scheme=$(cat "/tmp/corTestClient.$1.scheme")
  echo "$scheme://localhost:$1$2"
}


# ftClientStop [--port P]
#
# Stops the corTestClient on --port (default 7701). Safe to call when not running.
# Kills by port (pkill -f), not by pid file — a prior aborted test run may
# have left an orphan corTestClient whose pid file was since cleaned up;
# relying on the pid file would miss it and the new ftClientStart would
# silently fail to bind the port, letting the orphan handle requests
# with the wrong --status. See the distops tests (corTestClient status=503
# ended up served by a stale --status=201 instance).
#
ftClientStop() {
  local port=$FT_CLIENT_PORT
  while [ $# -gt 0 ]; do
    if [ "$1" == "--port" ] || [ "$1" == "-p" ]; then
      port="$2"
      shift
    fi
    shift
  done

  # Port-based kill — matches any corTestClient whose cmdline carries
  # "--port <port>". Harmless when no match.
  pkill -f "corTestClient.*--port $port( |\$)" 2>/dev/null
  sleep 0.1
  pkill -9 -f "corTestClient.*--port $port( |\$)" 2>/dev/null

  \rm -f /tmp/corTestClient.$port.pid
}


# ftClientDump [--port P] - retrieve accumulated notifications
#
# -----------------------------------------------------------------------------
#
# corPidAlive <pid> - is this process still RUNNING, as opposed to merely listed?
#
# `kill -0` cannot answer that, and believing it cost the nightly twenty hours.
#
# A zombie has exited. It stays in the process table only because nobody has reaped
# it, and `kill -0` on one SUCCEEDS - it is a valid pid that the signal check
# accepts. In a GitHub Actions job container nothing ever will reap it: the runner
# starts the container with `--entrypoint tail -f /dev/null`, so PID 1 is `tail`,
# which never calls wait(). Every orphan that exits there is a zombie forever.
#
# And the broker IS an orphan: the harness runs each test section in its own bash,
# so the shell that launched it has exited by the time the teardown runs. Locally
# this never showed, because PID 1 is systemd and reaps immediately.
#
# The symptom was the shape of a timeout, because it was one: the graceful-exit wait
# below ran its full 1200 x 0.1s on every single test - 120 seconds of watching a
# corpse - while a probe in the same image measured the real shutdown at 0.5-2.7s.
#
# So: read the state out of /proc instead. Field 3 of /proc/<pid>/stat is the state
# character, and Z means the work is done whatever the pid table says.
corPidAlive() {
  local pid=$1
  [ -n "$pid" ] || return 1
  [ -r "/proc/$pid/stat" ] || return 1
  local _p _c state
  #
  # 2>/dev/null BEFORE the input: redirections apply left to right, so with it after, a process that
  # exited between the -r test and the read had bash print "No such file or directory" first - on
  # stderr, which fails the test that was only stopping its broker (csource_reg_mongoc_persist, CI)
  #
  read -r _p _c state _ 2>/dev/null < "/proc/$pid/stat" || return 1   # a builtin - no fork in a polling loop
  [ "$state" != "Z" ]
}


# corValgrindSleep <seconds> - sleep ONLY when running under valgrind (--vt)
#
# Under valgrind the broker runs ~4-5x slower, so an async result (notably a
# notification delivered to corTestClient) may not have arrived by the time a test
# reads for it. This adds a settle delay on the valgrind path only; a normal run
# is unaffected and stays fast. Always returns 0.
#
corValgrindSleep() {
  [ "$COR_VALGRIND" == "1" ] && sleep "$1"
  return 0
}


ftClientDump() {
  # Let any in-flight notification land before reading (valgrind path only).
  # Also makes negative checks ("should NOT notify") robust: a late notification
  # would have arrived during the settle, so an empty dump is trustworthy.
  corValgrindSleep "${COR_VALGRIND_DUMP_SETTLE:-1.5}"

  local port=$FT_CLIENT_PORT
  while [ $# -gt 0 ]; do
    if [ "$1" == "--port" ] || [ "$1" == "-p" ]; then
      port="$2"
      shift
    fi
    shift
  done

  local raw
  raw=$(curl -sk "$(ftClientUrl $port /dump)")

  # A transient empty read (corTestClient momentarily unreachable under parallel
  # load) must still be valid JSON — emit "[]" so a downstream `json.load`/`jq`
  # never throws to stderr and flakes the test. For a real count, prefer
  # ftClientCount (reads /count, parser-free).
  [ -z "$raw" ] && raw="[]"

  if [ -n "$CORJSON" ] && [ -n "$raw" ] && [ "$raw" != "[]" ]; then
    echo "$raw" | $CORJSON -sort | head -c -1
  else
    echo -n "$raw"
  fi
}


# ftClientCount [--port P] - number of requests the mock receiver captured.
#
# Reads corTestClient's /count endpoint, which returns a bare integer (never JSON),
# so a caller never has to pipe a possibly-empty/invalid dump through a JSON
# parser — on an empty dump that parser throws to stderr and flakes the test
# under parallel load. Empty/failed read → 0.
ftClientCount() {
  # Same settle as ftClientDump so in-flight notifications are counted (valgrind
  # path only; a no-op otherwise).
  corValgrindSleep "${COR_VALGRIND_DUMP_SETTLE:-1.5}"

  local port=$FT_CLIENT_PORT
  while [ $# -gt 0 ]; do
    if [ "$1" == "--port" ] || [ "$1" == "-p" ]; then
      port="$2"
      shift
    fi
    shift
  done

  local n
  n=$(curl -sk "$(ftClientUrl $port /count)")
  echo "${n:-0}"
}


# ftClientWait <n> [--port P] - block until corTestClient has captured at least <n>
# requests, or until the (generous) deadline passes.
#
# Notifications are asynchronous, so the ORDER in which two of them land in the
# dump is the order the broker's notification threads happened to finish - not
# the order the triggering requests were sent. A test that triggers two of them
# back to back and then dumps is asserting on a coin flip; it comes up heads on
# a fast machine and tails under valgrind, which is how three of them went red
# in the nightly (subscription_type_star, json_property, csr_subscription_csf)
# while passing everywhere else.
#
# The fix is to make the order real: wait for notification 1 to have LANDED
# before sending the request that triggers notification 2. This polls /count
# (a bare integer, no JSON parser involved) rather than sleeping, so a normal
# run pays only the real latency - typically one 20 ms poll.
#
# Prints nothing: it is a barrier, not a step, and must not disturb the expect.
# Returns 0 if the count was reached, 1 on timeout (the caller's own assert then
# reports the real problem, rather than this hiding it).
#
ftClientWait() {
  local want=$1
  shift

  local port=$FT_CLIENT_PORT
  while [ $# -gt 0 ]; do
    if [ "$1" == "--port" ] || [ "$1" == "-p" ]; then
      port="$2"
      shift
    fi
    shift
  done

  # Under valgrind everything is ~5x slower, so the deadline is too. Both are
  # ceilings that a healthy run never approaches.
  local deadline=100                      # 100 x 0.02s = 2s
  [ "$COR_VALGRIND" == "1" ] && deadline=500   # 500 x 0.02s = 10s

  # An `if` rather than an `&&` chain on purpose: a chain whose last link fails
  # is a failing command, and this function is called as a plain statement.
  local n
  for ((i = 0; i < deadline; i++)); do
    n=$(curl -sk "$(ftClientUrl $port /count)")
    if [ -n "$n" ] && [ "$n" -eq "$n" ] 2>/dev/null && [ "$n" -ge "$want" ]; then
      return 0
    fi
    sleep 0.02
  done

  return 1
}


# ftClientProbeCount [--port P] - number of sourceIdentity discovery probes seen.
#
# Probes (GET .../info/sourceIdentity, § 5.15 alias discovery) are infrastructure
# and are kept OUT of the request dump, so ftClientCount never counts them. This
# reads corTestClient's /probeCount (bare integer) so a test can assert the probe fired
# (no contextSourceAlias supplied in the registration) or was skipped (alias given).
#
ftClientProbeCount() {
  local port=$FT_CLIENT_PORT
  while [ $# -gt 0 ]; do
    if [ "$1" == "--port" ] || [ "$1" == "-p" ]; then
      port="$2"
      shift
    fi
    shift
  done

  local n
  n=$(curl -sk "$(ftClientUrl $port /probeCount)")
  echo "${n:-0}"
}


# ftClientSettle [ms] [--port P] - the window in which NOTHING should arrive.
#
# The last thing a notification test does. `ftClientWait N` returns the instant
# the Nth notification lands and never looks for an N+1th, so a notification the
# broker should not have sent is invisible to it. This waits out the delivery
# window with nothing expected, and the ftClientDump that follows asserts the
# accumulator is empty - in the test's own --EXPECT--, where it can be read.
#
# Why 50 ms. Measured, not chosen: from "triggering response in hand" to
# "notification landed at corTestClient", over 1300 samples on an idle machine, on 32
# cores under 64 busy loops, and pinned to 2 contended CPUs, the worst observed
# was 5.9 ms and p99 was 4 ms. It stays small by construction - the deferred
# notification queue is per-connection and thread-local (ldNotifyDefer.c), so it
# is flushed inside the request rather than handed to a thread a loaded
# scheduler can starve. 50 ms is ~8x the worst measurement; it costs the suite
# about 6 seconds in total.
#
# Under valgrind, delivery is slower by the same factor as everything else.
#
ftClientSettle() {
  local ms=50
  local explicit=""
  [ -n "$1" ] && [ "$1" != "--port" ] && [ "$1" != "-p" ] && { ms="$1"; explicit=yes; shift; }

  # Only the DEFAULT scales under valgrind. A caller that names a number has
  # already decided what it is waiting for - multiplying it by 20 turned a 2 s
  # settle into 40 s and a 10 s test into 93 s.
  [ "$COR_VALGRIND" == "1" ] && [ -z "$explicit" ] && ms=$((ms * 20))

  sleep "$(awk "BEGIN { printf \"%.3f\", $ms / 1000 }")"
  return 0
}


# corHttpsCertGen [keyFile] [certFile] - generate a self-signed key + certificate
#
# For HTTPS-notification tests: corTestClient serves TLS with this pair and the broker
# (started with --insecureNotif) accepts the self-signed cert. Defaults to
# /tmp/corFtClient.key + /tmp/corFtClient.pem, CN=localhost. All openssl chatter
# goes to /dev/null so INIT stays stderr-clean.
#
corHttpsCertGen() {
  local keyFile=${1:-/tmp/corFtClient.key}
  local certFile=${2:-/tmp/corFtClient.pem}

  openssl genrsa -out "$keyFile" 2048 > /dev/null 2>&1
  openssl req -days 365 -new -x509 -key "$keyFile" -out "$certFile" \
          -subj "/C=ES/ST=Madrid/L=Madrid/O=Seamware/OU=test/CN=localhost/" > /dev/null 2>&1

  #
  # Readable by whoever ends up serving with them. mosquitto 2.x drops privileges
  # to the 'mosquitto' user when it is started as root - as it is in a container -
  # and openssl writes the key 0600 to the creating user, so the daemon cannot
  # read the key it was just handed and refuses to start. The symptom is an mqtts
  # test that delivers no notification, with nothing wrong anywhere near the
  # broker. Throwaway test material in /tmp, regenerated every run.
  #
  chmod 644 "$keyFile" "$certFile"
}


# -----------------------------------------------------------------------------
#
# Context Server (wistefan/context-server on port 7080)
#
CONTEXT_SERVER_PORT=${COR_CONTEXT_SERVER_PORT:-7080}

# contextServerReady - does something already answer on the context-server port?
#
# curl exit 0 means a response came back, whatever the HTTP code.
#
contextServerReady() {
  curl -s -o /dev/null --max-time 2 \
       "http://localhost:$CONTEXT_SERVER_PORT/jsonldContexts/_ready_probe" 2>/dev/null
}


# contextServerStart - ensure a context server is reachable on the port
#
# ASK BEFORE ACTING: if one already answers, use it. That is not just an
# optimisation - it is what lets the suite run where the server is provided
# rather than launched. A CI job declares it as a service container and there
# is no docker command inside the test container to start anything with; the
# old code went looking for docker first and failed before ever checking
# whether the thing it wanted was already there.
#
contextServerStart() {
  if contextServerReady; then
    return 0
  fi

  local dockerExec=$(which docker 2>/dev/null)
  if [ -z "$dockerExec" ]; then
    echo "contextServerStart: nothing answering on port $CONTEXT_SERVER_PORT, and no docker to start one"
    return 1
  fi

  local running=$(docker ps --filter name='^context-server$' -q 2>/dev/null)
  if [ -z "$running" ]; then
    docker run --rm -d --name context-server -p $CONTEXT_SERVER_PORT:8080 -e MEMORY_ENABLED=true wistefan/context-server > /dev/null 2>&1
  fi

  # Poll until the server actually accepts connections — a fixed sleep is racy:
  # the Java app's cold start can exceed it, and the immediately-following
  # contextServerPush then fails with curl exit 56 (INIT non-zero) even though
  # the container is "Up". curl exit 0 = a response came back (any HTTP code).
  local i
  for i in $(seq 1 30); do
    if curl -s -o /dev/null --max-time 2 "http://localhost:$CONTEXT_SERVER_PORT/jsonldContexts/_ready_probe" 2>/dev/null; then
      return 0
    fi
    sleep 1
  done
  echo "contextServerStart: context server not ready after 30s"
  return 1
}


# contextServerStop - stop the context server we manage, if there is one
#
# The decision is made from OBSERVABLE STATE, never from a variable: every
# section of a test (INIT, RUN, TEARDOWN) is extracted to its own script and
# executed as a separate process, so nothing set in INIT survives to here. A
# flag saying "we started it" is silently always false, the container is never
# stopped, and it accumulates every @context pushed by every test - which then
# leak into later tests as compaction that should not happen.
#
# So: a container of ours is one docker can see. If there is no docker (a CI
# job where the server is a service container) there is nothing of ours to
# stop, and the environment's server is left alone.
#
contextServerStop() {
  command -v docker > /dev/null 2>&1 || return 0
  docker kill context-server > /dev/null 2>&1
  return 0
}


# contextServerPush - push a JSON-LD context to the context server
#
# Usage: contextServerPush <url-path> '<json-ld-context>'
#
# A push DELETEs first, always. wistefan/context-server answers a re-POST to an
# existing path with the ORIGINAL body and no error, so a plain POST is a push
# that silently does not push whenever the server outlives the test.
#
# On a workstation it never showed: the harness starts the container and stops it
# again per test, so the server is always empty. In CI there is no docker to stop -
# the server is a service container living for the whole job - and the first test
# to use a path decided its content for every test after it. That surfaced as a
# notification compacted with another test's terms, which reads like a broker bug
# and is not one.
#
contextServerPush() {
  local urlPath="$1"
  local payload="$2"

  curl -s -X DELETE "http://localhost:$CONTEXT_SERVER_PORT$urlPath" > /dev/null
  curl -s -X POST "http://localhost:$CONTEXT_SERVER_PORT$urlPath" \
    -H 'Content-Type: application/ld+json' \
    -d "$payload" > /dev/null
}


# contextServerReplace - DELETE then POST so the context body can be updated.
# wistefan/context-server returns the original on a plain re-POST.
#
contextServerReplace() {
  local urlPath="$1"
  local payload="$2"

  curl -s -X DELETE "http://localhost:$CONTEXT_SERVER_PORT$urlPath" > /dev/null
  curl -s -X POST "http://localhost:$CONTEXT_SERVER_PORT$urlPath" \
    -H 'Content-Type: application/ld+json' \
    -d "$payload" > /dev/null
}



# -----------------------------------------------------------------------------
#
# ftModbus - a Modbus TCP device for the modbus bridge tests (tools/ftModbus.py)
#
# Modbus on FT_MODBUS_PORT, its control API on FT_MODBUS_PORT+1:
#
#   ftModbusStart                         start it (stops a previous one on the same port)
#   ftModbusStop
#   ftModbusSet <table> <addr> <v>...     set registers/bits: coil, discrete, holding, input
#   ftModbusGet <table> <addr> [count]    the values, as a JSON array
#   ftModbusMode normal|silent|exception [code]
#   ftModbusWrites                        how many write requests it has received
#
FT_MODBUS_PORT=7720

ftModbusStart() {
  ftModbusStop
  python3 "$(dirname "${BASH_SOURCE[0]}")/tools/ftModbus.py" --port $FT_MODBUS_PORT > /tmp/ftModbus.$FT_MODBUS_PORT.log 2>&1 &
  echo $! > /tmp/ftModbus.$FT_MODBUS_PORT.pid
  corAwaitPort $((FT_MODBUS_PORT + 1)) 10 > /dev/null
}

ftModbusStop() {
  local pidFile=/tmp/ftModbus.$FT_MODBUS_PORT.pid
  if [ -f $pidFile ]; then
    kill "$(cat $pidFile)" 2>/dev/null
    rm -f $pidFile
  fi
}



# -----------------------------------------------------------------------------
#
# ftWs - a WebSocket client for the WebSocket transport tests (tools/ftWs.py)
#
# ONE WebSocket to a broker (GET /ngsi-ld/v1/ws), kept open; its control API on FT_WS_PORT:

# -----------------------------------------------------------------------------
#
# ftExecutor - a Service Executor for the Service Execution tests (tools/ftExecutor.py): it answers
# the broker's invocations by path (/echo, /refuse, /async, ...) and receives notifications (/notify)
#
#   ftExecStart [brokerPort]   listen on FT_EXEC_PORT (7731); PATCH back to the broker on 'brokerPort' (default 1026)
#   ftExecStop
#   ftExecLog                  every request it received, a JSON array
#   ftExecNotifications        the notifications it received, a JSON array of their bodies
#
FT_EXEC_PORT=7731

ftExecStart() {
  local brokerPort=${1:-1026}
  ftExecStop
  python3 "$(dirname "${BASH_SOURCE[0]}")/tools/ftExecutor.py" --port $FT_EXEC_PORT --broker localhost:$brokerPort > /tmp/ftExecutor.$FT_EXEC_PORT.log 2>&1 &
  echo $! > /tmp/ftExecutor.$FT_EXEC_PORT.pid
  corAwaitPort $FT_EXEC_PORT 10 > /dev/null
}

ftExecStop() {
  local pidFile=/tmp/ftExecutor.$FT_EXEC_PORT.pid
  if [ -f $pidFile ]; then
    kill "$(cat $pidFile)" 2>/dev/null
    rm -f $pidFile
  fi
}

ftExecLog()           { curl -s localhost:$FT_EXEC_PORT/log; }
ftExecNotifications() { curl -s localhost:$FT_EXEC_PORT/notifications; }

#
#   ftWsStart [port] [protocol] [first] [tenant]
#                                         connect to the broker on 'port' (default 1026, CB); 'first': a
#                                         message sent with the upgrade request, before the 101; 'tenant':
#                                         the upgrade request's NGSILD-Tenant
#   ftWsStop
#   ftWsSend <json>                       one text message
#   ftWsSendFrag <json>                   the same, as a text frame and a continuation
#   ftWsPing <payload>
#   ftWsClose                             a close frame (1000)
#   ftWsMessages                          every message received so far, a JSON array
#   ftWsAwait <n> [seconds]               until n messages have arrived (default 5 s)
#
FT_WS_PORT=7730

ftWsStart() {
  local brokerPort=${1:-1026}
  local protocol=$2
  local first=$3                        # a message sent with the upgrade request, before the 101
  local tenant=$4
  ftWsStop
  python3 "$(dirname "${BASH_SOURCE[0]}")/tools/ftWs.py" --broker localhost:$brokerPort --port $FT_WS_PORT ${protocol:+--protocol $protocol} ${first:+--first "$first"} ${tenant:+--tenant $tenant} > /tmp/ftWs.$FT_WS_PORT.log 2>&1 &
  echo $! > /tmp/ftWs.$FT_WS_PORT.pid
  corAwaitPort $FT_WS_PORT 10 > /dev/null
}

ftWsStop() {
  local pidFile=/tmp/ftWs.$FT_WS_PORT.pid
  if [ -f $pidFile ]; then
    kill "$(cat $pidFile)" 2>/dev/null
    rm -f $pidFile
  fi
}

ftWsSend()     { curl -s -X POST localhost:$FT_WS_PORT/send     --data-binary "$1"; }
ftWsSendFrag() { curl -s -X POST localhost:$FT_WS_PORT/sendFrag --data-binary "$1"; }
ftWsPing()     { curl -s -X POST localhost:$FT_WS_PORT/ping     --data-binary "$1"; }
ftWsClose()    { curl -s -X POST localhost:$FT_WS_PORT/close; }
ftWsMessages() { curl -s localhost:$FT_WS_PORT/messages; }

ftWsAwait() {
  local n=$1
  local tries=$(( ${2:-5} * 20 ))
  for i in $(seq 1 $tries); do
    [ "$(curl -s localhost:$FT_WS_PORT/count)" -ge "$n" ] 2>/dev/null && return 0
    sleep 0.05
  done
  echo "ftWsAwait: $(curl -s localhost:$FT_WS_PORT/count) of $n messages"
  return 1
}

ftModbusSet() {
  local table=$1 addr=$2
  shift 2
  local values=$(IFS=,; echo "$*")
  curl -s -o /dev/null -X POST "http://localhost:$((FT_MODBUS_PORT + 1))/set" \
       -d "{\"table\":\"$table\",\"addr\":$addr,\"values\":[$values]}"
}

ftModbusGet() {
  curl -s "http://localhost:$((FT_MODBUS_PORT + 1))/get?table=$1&addr=$2&count=${3:-1}"
  echo
}

ftModbusMode() {
  curl -s -o /dev/null -X POST "http://localhost:$((FT_MODBUS_PORT + 1))/mode" -d "{\"mode\":\"$1\",\"code\":${2:-2}}"
}

ftModbusWrites() {
  curl -s "http://localhost:$((FT_MODBUS_PORT + 1))/writes"
  echo
}



# -----------------------------------------------------------------------------
#
# bridgeConfig - write a bridge configuration file
#
# Usage:  bridgeConfig [-o <file>] [-b <bridge alias>]
#                      [--topic "<endpoint>,<entityType>,<entityId>,<attribute>"] ...
#                      [--emit  "<endpoint>=<json value>"] ...
#                      [--raw   "<verbatim json member>"] ...
#
# Writes to /tmp/coraine_bridges.json unless -o says otherwise, and echoes the
# path, so a test can say:
#
#   coraineStart --bridges loopback --bridgeConfig $(bridgeConfig \
#       --topic "P1,Camera,urn:ngsi-ld:camera:cam1,shutterSpeed" \
#       --topic "P2,Arm,urn:ngsi-ld:arm:arm1,armReach")
#
# The four fields of --topic are in the order the file itself reads in, so the
# line can be checked against the JSON without translating it.
#
# --emit queues a sample on an endpoint at startup. Only the loopback bridge
# honours it, that being the point of the loopback bridge: it makes an arriving
# value testable without a transport, a publisher or a network.
#
# --raw drops a member into the bridge's object verbatim, for the transport's
# own settings - which the broker never reads, and which a test therefore only
# ever needs in order to prove they are ignored.
#
# --typesDirectory names where the plugin keeps type bytes (ngsild.typesDirectory).
# Only a SERVER needs one given to it: it announces a service, and the transport
# asks for the service's types before anybody else has said a word about them.
#
# --replyDelay "<endpoint>=<ms>" makes the loopback bridge answer a service that
# late, and -1 never - what a test of a request that WAITS for its reply
# (ddsSync) needs to produce a timeout, and a reply after one, on purpose.
#
bridgeConfig() {
  local outFile="/tmp/coraine_bridges.json"
  local bridge="loopback"
  local defaultEntity=""
  local typesDirectory=""
  local syncTimeoutMs=""
  local -a topics
  local -a services
  local -a actions
  local -a emits
  local -a replyDelays
  local -a goalModes
  local -a discovers
  local -a topicModes
  local -a actionNotifies
  local -a metas
  local -a echoRequests
  local bridgeNotify=""
  local -a raws

  while [ $# -gt 0 ]; do
    case "$1" in
      -o)       outFile="$2"; shift ;;
      -b)       bridge="$2";  shift ;;
      --typesDirectory) typesDirectory="$2"; shift ;;
      --syncTimeoutMs) syncTimeoutMs="$2"; shift ;;    # dds.ngsild.syncTimeoutMs, as Orion-LD reads it
      --topic)  topics+=("$2"); shift ;;
      #
      # A service entry is a topic entry plus, optionally, the names of its two
      # DDS types - which the broker never reads and the plugin always does.
      #   <endpoint>,<entityType>,<entityId>,<attribute>[,<requestType>,<replyType>]
      #
      --service) services+=("$2"); shift ;;
      #
      # An action entry has the shape of a service entry, its optional fifth field
      # the DDS action type (dds bridge only). Writing its attribute
      # sends a goal; --goalMode "<endpoint>=succeed|hold|reject|abort" tells the
      # loopback bridge what to make of the goals sent there (succeed if unsaid).
      #
      --action) actions+=("$2"); shift ;;
      --goalMode) goalModes+=("$2"); shift ;;
      --discover) discovers+=("$2"); shift ;;    # loopback: "<endpoint>=service|action" - reported as discovered (ABI 8)
      #
      # --topicMode "<endpoint>=accept|refuse|unannounced" - what the loopback
      # bridge answers a sample published there (accept if unsaid): refuse is a
      # value that does not fit (400), unannounced one that cannot be sent (503)
      #
      --topicMode) topicModes+=("$2"); shift ;;
      #
      # Default goal endpoints, for goals that name none: --actionNotify
      # "<endpoint>=<uri>" is that action Channel's, --notify "<uri>" the
      # Bridge's (ngsild.notification). The goal's own endpoint wins, then the
      # Channel's, then the Bridge's.
      #
      #
      # --meta "<endpoint>=<json object>": what the loopback says ABOUT every
      # payload on that endpoint (ABI 6) - each member becomes a Property
      # sub-attribute of whatever the payload lands in.
      #
      --meta) metas+=("$2"); shift ;;
      #
      # --echoRequest "<endpoint>=<sub-attribute>": a reply on that service goes
      # back with the request it answers, under that sub-attribute (ABI 7)
      #
      --echoRequest) echoRequests+=("$2"); shift ;;
      --actionNotify) actionNotifies+=("$2"); shift ;;
      --notify) bridgeNotify="$2"; shift ;;
      #
      # The catch-all entity: "true" for the derived one, or "<id>,<type>" to
      # name it. An endpoint no topic claims goes there instead of being
      # dropped - which is off unless the file says so.
      #
      --defaultEntity) defaultEntity="$2"; shift ;;
      --emit)   emits+=("$2");  shift ;;
      --replyDelay) replyDelays+=("$2"); shift ;;
      --raw)    raws+=("$2");   shift ;;
      *)        echo "bridgeConfig: unknown option '$1'" >&2; return 1 ;;
    esac
    shift
  done

  {
    echo "{"
    echo "  \"$bridge\": {"

    local r
    for r in "${raws[@]}"; do
      echo "    $r,"
    done

    if [ ${#emits[@]} -gt 0 ]; then
      echo "    \"emitAtStart\": {"
      local i=0
      local e
      for e in "${emits[@]}"; do
        local endpoint="${e%%=*}"
        local value="${e#*=}"
        i=$((i + 1))
        if [ $i -lt ${#emits[@]} ]; then
          echo "      \"$endpoint\": $value,"
        else
          echo "      \"$endpoint\": $value"
        fi
      done
      echo "    },"
    fi

    if [ ${#metas[@]} -gt 0 ]; then
      echo "    \"meta\": {"
      local i=0
      local m
      for m in "${metas[@]}"; do
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#metas[@]} ] && comma=""
        echo "      \"${m%%=*}\": ${m#*=}$comma"
      done
      echo "    },"
    fi

    if [ ${#echoRequests[@]} -gt 0 ]; then
      echo "    \"echoRequest\": {"
      local i=0
      local er
      for er in "${echoRequests[@]}"; do
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#echoRequests[@]} ] && comma=""
        echo "      \"${er%%=*}\": \"${er#*=}\"$comma"
      done
      echo "    },"
    fi

    if [ ${#replyDelays[@]} -gt 0 ]; then
      echo "    \"replyDelayMs\": {"
      local i=0
      local d
      for d in "${replyDelays[@]}"; do
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#replyDelays[@]} ] && comma=""
        echo "      \"${d%%=*}\": ${d#*=}$comma"
      done
      echo "    },"
    fi

    if [ ${#discovers[@]} -gt 0 ]; then
      echo "    \"discover\": {"
      local i=0
      local d
      for d in "${discovers[@]}"; do
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#discovers[@]} ] && comma=""
        echo "      \"${d%%=*}\": \"${d#*=}\"$comma"
      done
      echo "    },"
    fi

    if [ ${#goalModes[@]} -gt 0 ]; then
      echo "    \"goalMode\": {"
      local i=0
      local g
      for g in "${goalModes[@]}"; do
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#goalModes[@]} ] && comma=""
        echo "      \"${g%%=*}\": \"${g#*=}\"$comma"
      done
      echo "    },"
    fi

    if [ ${#topicModes[@]} -gt 0 ]; then
      echo "    \"topicMode\": {"
      local i=0
      local g
      for g in "${topicModes[@]}"; do
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#topicModes[@]} ] && comma=""
        echo "      \"${g%%=*}\": \"${g#*=}\"$comma"
      done
      echo "    },"
    fi

    echo "    \"ngsild\": {"

    if [ -n "$syncTimeoutMs" ]; then
      echo "      \"syncTimeoutMs\": $syncTimeoutMs,"
    fi

    if [ -n "$typesDirectory" ]; then
      echo "      \"typesDirectory\": \"$typesDirectory\","
    fi

    if [ -n "$bridgeNotify" ]; then
      echo "      \"notification\": { \"endpoint\": { \"uri\": \"$bridgeNotify\" } },"
    fi

    if [ -n "$defaultEntity" ]; then
      if [ "$defaultEntity" == "true" ] || [ "$defaultEntity" == "false" ]; then
        echo "      \"defaultEntity\": $defaultEntity,"
      else
        local deId deType
        IFS=',' read -r deId deType <<< "$defaultEntity"
        echo "      \"defaultEntity\": { \"id\": \"$deId\", \"type\": \"$deType\" },"
      fi
    fi

    echo "      \"topics\": {"

    local i=0
    local t
    for t in "${topics[@]}"; do
      local endpoint entityType entityId attribute
      IFS=',' read -r endpoint entityType entityId attribute <<< "$t"
      i=$((i + 1))
      local comma=","
      [ $i -eq ${#topics[@]} ] && comma=""
      #
      # An entry missing its attribute is written as given - a test that wants
      # to prove a half-written entry is skipped has to be able to write one.
      #
      if [ -n "$attribute" ]; then
        echo "        \"$endpoint\": { \"entityId\": \"$entityId\", \"entityType\": \"$entityType\", \"attribute\": \"$attribute\" }$comma"
      else
        echo "        \"$endpoint\": { \"entityId\": \"$entityId\", \"entityType\": \"$entityType\" }$comma"
      fi
    done

    if [ ${#services[@]} -eq 0 ]; then
      echo "      }"
    else
      echo "      },"
      echo "      \"services\": {"

      i=0
      local sv
      for sv in "${services[@]}"; do
        local sEndpoint sType sId sAttr sReq sRep
        IFS=',' read -r sEndpoint sType sId sAttr sReq sRep <<< "$sv"
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#services[@]} ] && comma=""

        local types=""
        [ -n "$sReq" ] && types="$types, \"requestType\": \"$sReq\""
        [ -n "$sRep" ] && types="$types, \"replyType\": \"$sRep\""

        echo "        \"$sEndpoint\": { \"entityId\": \"$sId\", \"entityType\": \"$sType\", \"attribute\": \"$sAttr\"$types }$comma"
      done

      echo "      }"
    fi

    #
    # Actions, if any - after whichever section came last, so the comma that
    # separates them leads this one.
    #
    if [ ${#actions[@]} -gt 0 ]; then
      echo "      ,\"actions\": {"
      i=0
      local av
      for av in "${actions[@]}"; do
        local aEndpoint aType aId aAttr aActionType
        IFS=',' read -r aEndpoint aType aId aAttr aActionType <<< "$av"
        i=$((i + 1))
        local comma=","
        [ $i -eq ${#actions[@]} ] && comma=""
        #
        # The fifth field is the DDS action type - the broker never reads it,
        # the dds plugin derives every one of the action's types from it.
        #
        local actionType=""
        [ -n "$aActionType" ] && actionType=", \"type\": \"$aActionType\""
        local notify=""
        local an
        for an in "${actionNotifies[@]}"; do
          [ "${an%%=*}" == "$aEndpoint" ] && notify=", \"notification\": { \"endpoint\": { \"uri\": \"${an#*=}\" } }"
        done
        echo "        \"$aEndpoint\": { \"entityId\": \"$aId\", \"entityType\": \"$aType\", \"attribute\": \"$aAttr\"$actionType$notify }$comma"
      done
      echo "      }"
    fi

    echo "    }"
    echo "  }"
    echo "}"
  } > "$outFile"

  echo "$outFile"
}


# -----------------------------------------------------------------------------
#
# ros2NodeStart / ros2NodeStop / ros2NodeWait - a REAL DDS publisher, in a container
#
# The DDS tests need a participant that is not ours, and the reason is the whole
# reason these helpers exist: a DDS participant's topics are compiled into it.
# Nobody subscribes to a topic the broker invents, so two instances of our own
# plugin can never bootstrap each other, and the loopback bridge can only ever
# prove the SHAPE of a sample crossing.
#
# The publisher is the ROS 2 demo talker in eprosima/vulcanexus - the same image
# and the same node the orion-ld DDS tests use. ROS 2's /chatter is DDS topic
# `rt/chatter` (ROS prefixes topics with `rt/`) carrying std_msgs::msg::String_,
# and the mirror is the demo listener, which is how an outbound sample is
# observed without a second broker.
#
# ⚠ --ipc=host, and NOT --net=host.
#
# Fast DDS reaches a participant on the same machine over SHARED MEMORY, so the
# container has to share /dev/shm with the broker - without it the broker
# discovers nothing at all and no error is printed anywhere. With BOTH flags,
# discovery gets confused and the topics never surface either. One, not two.
#
# The image is never pulled here. It is 6.5 GiB, and whether it is present is
# what -dds detects (corTestParams.sh).
#
COR_DDS_ROS2_IMAGE="${COR_DDS_ROS2_IMAGE:-eprosima/vulcanexus:jazzy-desktop}"
COR_DDS_DOMAIN="${COR_DDS_DOMAIN:-0}"


# ros2NodeStart <talker|listener> - start a ROS 2 demo node on the DDS domain
#
ros2NodeStart() {
  local node="$1"
  local name="cor_ros2_$node"

  docker rm -f "$name" > /dev/null 2>&1

  docker run --rm -d --name "$name" --ipc=host -e ROS_DOMAIN_ID="$COR_DDS_DOMAIN" \
         "$COR_DDS_ROS2_IMAGE" \
         bash -c 'exec python3 /opt/ros/$ROS_DISTRO/lib/demo_nodes_py/'"$node" > /dev/null 2>&1 \
    || { echo "ros2NodeStart: could not start the ROS 2 $node" >&2; return 1; }

  #
  # A node that dies on startup leaves a container that is simply gone, and the
  # test after it then waits for a sample nobody is publishing. Ask once.
  #
  ros2NodeWait "$node" "." 10 \
    || { echo "ros2NodeStart: the ROS 2 $node printed nothing in 10s" >&2; return 1; }
}


# ros2ServiceStart <node> <service> [seconds] - start a ROS 2 service SERVER
#
# ⭐ NOT ros2NodeStart, and the difference is the whole point. That one waits
# for the node's first log line, which is right for a talker and impossible for
# a server: a service server says nothing at all until somebody asks it
# something, so there is no first line to wait for.
#
# What a test actually depends on is something else anyway - that the BROKER has
# DISCOVERED the service. An invocation sent before that has no server to reach
# and is refused outright, which is not a race worth having in a test. So this
# waits for the service's Channel to say it was discovered (ddsServiceAwait).
#
ros2ServiceStart() {
  local node="$1"
  local service="$2"
  local secs="${3:-30}"
  local name="cor_ros2_$node"

  docker rm -f "$name" > /dev/null 2>&1

  docker run --rm -d --name "$name" --ipc=host -e ROS_DOMAIN_ID="$COR_DDS_DOMAIN" \
         "$COR_DDS_ROS2_IMAGE" \
         bash -c 'exec python3 /opt/ros/$ROS_DISTRO/lib/demo_nodes_py/'"$node" > /dev/null 2>&1 \
    || { echo "ros2ServiceStart: could not start the ROS 2 $node" >&2; return 1; }

  ddsServiceAwait "$service" "$secs"
}


# bridgeChannelGet <bridge> <endpoint> <member> - one member of the Channel that carries <endpoint> on <bridge>
#
# From GET /channels (the default tenant), found by bridge and endpoint - a Channel's id may be the one
# its configuration gave it. Prints the value (true/false for a boolean), nothing when there is no such
# Channel or it has no such member.
#
# ⭐ A TEST WAITS ON WHAT THE API SAYS, NEVER ON THE LOG. A trace line is there only when the trace
# levels include it AND the plugin was built with its traces - a dds.so with them compiled out timed
# out every bridge_dds_* test while the broker was working fine (2026-09-29).
#
bridgeChannelGet() {
  curl -s "localhost:$CB_PORT/ngsi-ld/v1/channels" 2>/dev/null | python3 -c '
import json, sys
bridge, endpoint, member = sys.argv[1:4]
try:
  channels = json.load(sys.stdin)
except Exception:
  sys.exit(0)
for c in channels:
  if c.get("bridgeId") == "urn:ngsi-ld:ContextBridge:" + bridge and c.get("channelTarget") == endpoint and member in c:
    v = c[member]
    print(("true" if v else "false") if isinstance(v, bool) else v)
    break
' "$1" "$2" "$3"
}


# bridgeGet <bridge> <member> - one member of a ContextBridge, from GET /bridges/{id}; nothing when absent
#
bridgeGet() {
  curl -s "localhost:$CB_PORT/ngsi-ld/v1/bridges/urn:ngsi-ld:ContextBridge:$1" 2>/dev/null | python3 -c '
import json, sys
try:
  b = json.load(sys.stdin)
except Exception:
  sys.exit(0)
if sys.argv[1] in b:
  print(b[sys.argv[1]])
' "$2"
}


# bridgeAtLeast <bridge> <member> <n> [seconds] - wait until that counter of the ContextBridge is at least n
#
# samplesDropped: samples no Channel claims that would write what a Channel already writes (a catch-all's
# collision) - NOT the samples nothing claims at all.
#
bridgeAtLeast() {
  local bridge="$1" member="$2" n="$3" secs="${4:-10}"
  local i v

  for ((i = 0; i < secs * 10; i++)); do
    v=$(bridgeGet "$bridge" "$member")
    [ -n "$v" ] && [ "$v" -ge "$n" ] && return 0
    sleep 0.1
  done

  echo "bridgeAtLeast: bridge '$bridge': $member is '$v', not >= $n, after ${secs}s" >&2
  return 1
}


# bridgeChannelsCount <bridge> <member> -"<endpoint>: <n>" for every Channel of <bridge> whose counter is not 0
#
# Ordered by endpoint (P2 before P10). samplesOut: the values each endpoint published - a write that
# published twice, or an echo that came back out, shows as a 2.
#
bridgeChannelsCount() {
  curl -s "localhost:$CB_PORT/ngsi-ld/v1/channels" 2>/dev/null | python3 -c '
import json, re, sys
bridge, member = sys.argv[1:3]
try:
  channels = json.load(sys.stdin)
except Exception:
  sys.exit(0)
rows = [(c["channelTarget"], c[member]) for c in channels
        if c.get("bridgeId") == "urn:ngsi-ld:ContextBridge:" + bridge and c.get(member, 0) != 0]
for endpoint, n in sorted(rows, key=lambda r: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", r[0])]):
  print("%s: %s" % (endpoint, n))
' "$1" "$2"
}


# bridgeChannelAwait <bridge> <endpoint> <member> <value> [seconds] - wait until that member of the Channel has that value
#
bridgeChannelAwait() {
  local bridge="$1" endpoint="$2" member="$3" value="$4" secs="${5:-30}"
  local i

  for ((i = 0; i < secs * 10; i++)); do
    [ "$(bridgeChannelGet "$bridge" "$endpoint" "$member")" == "$value" ] && return 0
    sleep 0.1
  done

  echo "bridgeChannelAwait: channel '$endpoint' on '$bridge': $member is '$(bridgeChannelGet "$bridge" "$endpoint" "$member")', not '$value', after ${secs}s" >&2
  return 1
}


# bridgeChannelAtLeast <bridge> <endpoint> <member> <n> [seconds] - wait until that counter of the Channel is at least n
#
bridgeChannelAtLeast() {
  local bridge="$1" endpoint="$2" member="$3" n="$4" secs="${5:-30}"
  local i v

  for ((i = 0; i < secs * 10; i++)); do
    v=$(bridgeChannelGet "$bridge" "$endpoint" "$member")
    [ -n "$v" ] && [ "$v" -ge "$n" ] && return 0
    sleep 0.1
  done

  echo "bridgeChannelAtLeast: channel '$endpoint' on '$bridge': $member is '$v', not >= $n, after ${secs}s" >&2
  return 1
}


# bridgeGoalInProgress <bridge> <endpoint> <goalId> - "yes" while the goal is in progress on its Channel, else "no"
#
# GET /channels/{id}/goals/{goalId}: 200 from the moment the goal is sent until its final event, 404
# after (and for a goal never sent).
#
bridgeGoalInProgress() {
  local channelId
  channelId=$(bridgeChannelGet "$1" "$2" id)
  [ -z "$channelId" ] && { echo "no"; return; }
  if [ "$(curl -s -o /dev/null -w '%{http_code}' "localhost:$CB_PORT/ngsi-ld/v1/channels/$channelId/goals/$3")" == "200" ]; then
    echo "yes"
  else
    echo "no"
  fi
}


# bridgeGoalEndedAwait <bridge> <endpoint> <goalId> [seconds] - wait until a goal sent has ended (its final event arrived)
#
bridgeGoalEndedAwait() {
  local secs="${4:-10}"
  local i

  for ((i = 0; i < secs * 10; i++)); do
    [ "$(bridgeGoalInProgress "$1" "$2" "$3")" == "no" ] && return 0
    sleep 0.1
  done

  echo "bridgeGoalEndedAwait: goal '$3' on '$2' (bridge '$1') still in progress after ${secs}s" >&2
  return 1
}


# attrMemberAwait <entityId> <attr> <member> [datasetId] [seconds] - wait until the attribute (the instance of that
# datasetId, if given) has that member - a sub-attribute a bridge wrote ('status', 'feedback', 'reply', ...)
#
# From GET /entities/{id} in the default tenant, normalized; <attr> and <member> as the core @context names them.
#
attrMemberAwait() {
  local entityId="$1" attr="$2" member="$3" datasetId="$4" secs="${5:-10}"
  local i

  for ((i = 0; i < secs * 10; i++)); do
    curl -s "localhost:$CB_PORT/ngsi-ld/v1/entities/$entityId" 2>/dev/null | python3 -c '
import json, sys
attr, member, datasetId = sys.argv[1:4]
try:
  e = json.load(sys.stdin)
except Exception:
  sys.exit(1)
a = e.get(attr)
instances = a if isinstance(a, list) else ([a] if isinstance(a, dict) else [])
for i in instances:
  if (datasetId == "" or i.get("datasetId") == datasetId) and member in i:
    sys.exit(0)
sys.exit(1)
' "$attr" "$member" "$datasetId" && return 0
    sleep 0.1
  done

  echo "attrMemberAwait: $entityId/$attr${datasetId:+ (datasetId $datasetId)} has no '$member' after ${secs}s" >&2
  return 1
}


# bridgeGoalHistory <entityId> <attr> <goalAlias> [prefix] - how a goal went, from the attribute's temporal history
#
# A goal's instance (datasetId = its alias, urn:goal:N) is removed after its final event, so how it
# ended is read where it stays: GET /temporal/entities (needs --troe timescale; --troeSync for it to be
# there at once). Prints the status codes in order (each once), how many feedbacks (when any), whether
# a result was written, and whether the instance was then removed - or that the goal left no history
# (refused before anything was written). Asked for in modifiedAt order (timerel + timeproperty): a goal's
# instances have no observedAt, and without a time property the order is not one.
#
# [prefix]: what the bridge names its members with - dds: ddsAction (ddsActionStatus, ddsActionFeedback,
# ddsActionResult); none: status, feedback, result.
#
bridgeGoalHistory() {
  curl -s "localhost:$CB_PORT/ngsi-ld/v1/temporal/entities/$1?attrs=$2&timerel=after&timeAt=1970-01-01T00:00:00Z&timeproperty=modifiedAt" 2>/dev/null | python3 -c '
import json, sys
attr, alias, prefix = sys.argv[1:4]
name = lambda m: prefix + m.capitalize() if prefix else m
try:
  e = json.load(sys.stdin)
except Exception:
  print("no temporal entity"); sys.exit(0)
inst = e.get(attr, [])
inst = inst if isinstance(inst, list) else [inst]
mine = [i for i in inst if i.get("datasetId") == alias]
if not mine:
  print("%s: no history" % alias); sys.exit(0)
codes    = []
feedback = []
for i in mine:
  st = i.get(name("status"), {}).get("value")
  code = st.get("code") if isinstance(st, dict) else st
  if code is not None and (not codes or codes[-1] != code):
    codes.append(code)
  fb = i.get(name("feedback"), {}).get("value")
  if fb is not None and (not feedback or feedback[-1] != fb):
    feedback.append(fb)
result  = any(name("result") in i for i in mine)
removed = "deletedAt" in mine[-1]
print("%s: %s%s, result %s, instance %s" % (alias, " -> ".join(codes) if codes else "no status",
      ", feedback %d" % len(feedback) if feedback else "", "written" if result else "none", "removed" if removed else "still there"))
' "$2" "$3" "$4"
}


# bridgeGoalHistoryAwait <entityId> <attr> <goalAlias> [prefix] [seconds] - bridgeGoalHistory, once the goal has ended
#
# Ended = its instance removed, in the history. For a goal on an endpoint with no Channel to ask
# (bridgeGoalEndedAwait) - one the catch-all carries.
#
bridgeGoalHistoryAwait() {
  local secs="${5:-20}"
  local i h

  for ((i = 0; i < secs * 10; i++)); do
    h=$(bridgeGoalHistory "$1" "$2" "$3" "$4")
    [[ "$h" == *"instance removed" ]] && { echo "$h"; return 0; }
    sleep 0.1
  done

  echo "bridgeGoalHistoryAwait: goal $3 on $1/$2 has not ended after ${secs}s: $h" >&2
  return 1
}


# attrDatasetIds <entityId> <attr> - the datasetIds of the attribute's instances, one per line (a goal's: urn:goal:<id>)
#
attrDatasetIds() {
  curl -s "localhost:$CB_PORT/ngsi-ld/v1/entities/$1" 2>/dev/null | python3 -c '
import json, sys
try:
  a = json.load(sys.stdin).get(sys.argv[1])
except Exception:
  sys.exit(0)
for i in (a if isinstance(a, list) else [a] if isinstance(a, dict) else []):
  if "datasetId" in i:
    print(i["datasetId"])
' "$2"
}


# ddsServiceAwait <service> [seconds] - wait until the broker's DDS bridge has discovered a service
#
# Whatever serves it - a ROS 2 node (ros2ServiceStart) or corTestClient - a request sent before it is
# discovered has no server to reach and is refused outright. Discovered = its Channel says so
# (endpointDiscovered), not a trace line.
#
ddsServiceAwait() {
  bridgeChannelAwait dds "$1" endpointDiscovered true "${2:-30}" || { echo "ddsServiceAwait: the broker did not discover service '$1' in ${2:-30}s" >&2; return 1; }
}


# ros2NodeStop [<node> ...] - stop the nodes (default: both). Safe when not running.
#
ros2NodeStop() {
  local nodes="$*"
  local node

  [ -z "$nodes" ] && nodes="talker listener"

  for node in $nodes; do
    docker rm -f "cor_ros2_$node" > /dev/null 2>&1
  done

  return 0
}


# ros2NodeWait <node> <extended regex> [seconds] - block until the node logs it
#
# Returns 1 and says what it waited for, rather than leaving the caller to
# assert on an empty log and report the absence as a payload difference.
#
ros2NodeWait() {
  local node="$1"
  local pattern="$2"
  local secs="${3:-10}"
  local i

  for ((i = 0; i < secs * 10; i++)); do
    docker logs "cor_ros2_$node" 2>&1 | grep -qE "$pattern" && return 0
    sleep 0.1
  done

  echo "ros2NodeWait: the ROS 2 $node did not log '$pattern' in ${secs}s" >&2
  return 1
}


# ros2NodeHeard <extended regex> - what the listener heard, matching lines only
#
# The listener's own lines carry a node timestamp ("[INFO] [1790095535.989...]
# [listener]: I heard: [...]"), which is not the test's business and cannot be
# asserted on - so only the matching part is printed.
#
ros2NodeHeard() {
  docker logs cor_ros2_listener 2>&1 | grep -oE "$1"
}
