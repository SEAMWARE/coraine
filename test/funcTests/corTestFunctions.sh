# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - repo-specific test functions for coraine
#
export COR_BROKER="${COR_BROKER:-coraine}"        # broker from PATH (installed via make di)
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

# Plugins from their install site; ftClient from the repo (cmake builds it there).
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

  # Current-state DB plugin
  case "$COR_DB_TYPE" in
    mongoc) cmd="$cmd --database $COR_PLUGIN_DIR/db/currentState/mongoc.so --dbName $COR_ROLE_DB_PREFIX --dbHost $COR_MONGO_HOST --dbPort $COR_MONGO_PORT" ;;
    corDB)  cmd="$cmd --database $COR_PLUGIN_DIR/db/currentState/corDB.so" ;;
    NONE)   ;;  # compiled-in default
    *)      echo "coraineStart: unknown -db type: $COR_DB_TYPE"; return 1 ;;
  esac

  # TRoE DB plugin (future)
  case "$COR_TROE_DB_TYPE" in
    NONE|"") ;;  # compiled-in default or unset
    *)       echo "coraineStart: unknown -troeDb type: $COR_TROE_DB_TYPE"; return 1 ;;
  esac

  # Timescale TRoE convenience: when a test asks for "--troe timescale" without
  # naming the DB, derive the role-keyed name (corh_<role>) — the same name
  # corTroeInit/corTroeDrop create/drop — and add --troeUser. Tests that pass an
  # explicit --troeName keep full control.
  if printf '%s\n' "${extraParams[@]}" | grep -qx 'timescale' && \
     ! printf '%s\n' "${extraParams[@]}" | grep -qx -- '--troeName'; then
    extraParams+=(--troeName "$(corTroeDbName "$role")" --troeUser "$COR_TROE_USER" --troeHost "$COR_TROE_HOST" --troePort "$COR_TROE_PORT")
  fi

  # Append test-specific extra params
  if [ ${#extraParams[@]} -gt 0 ]; then
    cmd="$cmd ${extraParams[*]}"
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

  # Port-based kill so orphans from aborted prior runs (with no live pid
  # file) are still caught. Matches any coraine whose cmdline carries
  # "--port <port>".
  local pat="coraine.*--port $COR_ROLE_PORT( |\$)"
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
      if [ -n "$explicitDb" ]; then
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
    corDB|NONE)
      # No-op: broker restart clears the RAM store
      ;;
  esac
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
      mongosh --host $COR_MONGO_HOST --port $COR_MONGO_PORT --quiet --eval \
        "db.adminCommand('listDatabases').databases \
          .map(d=>d.name) \
          .filter(n=>n.startsWith(\"$rolePrefix\")&&n.includes(\"-_snap_\")) \
          .forEach(n=>db.getSiblingDB(n).dropDatabase())" > /dev/null 2>&1
      ;;
    corDB|NONE)
      ;;
  esac
}


# -----------------------------------------------------------------------------
#
# ftClient - generic mock endpoint for forward-target / notification-receiver tests
#
# Each instance has its own PID file keyed by port, so multiple ftClients
# can run concurrently (one per CSR target) and be stopped individually.
#
FT_CLIENT=$COR_REPO_DIR/test/funcTests/ftClient/ftClient
FT_CLIENT_PORT=7701                          # default port when none given


# ftClientStart [--port P] [--status S] [...extra]
#
# Starts an ftClient on the given port (default 7701). --status sets the
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

  local pidFile=/tmp/ftClient.$port.pid
  ftClientStop --port $port 2>/dev/null

  # Record the scheme so ftClientDump/ftClientCount reach the right URL: a
  # --httpsKey/--httpsCertificate ftClient serves HTTPS, plain HTTP otherwise.
  local scheme=http
  case " ${extraParams[*]} " in
    *" --httpsKey "*|*" -k "*) scheme=https ;;
  esac
  echo "$scheme" > /tmp/ftClient.$port.scheme

  #
  # Keep what it says. When an ftClient cannot start - no TLS in the HTTP library,
  # a port already taken, a missing certificate - the only symptom used to be
  # "corAwaitPort: port N not ready", because its stderr went to /dev/null. The
  # reason was always one line long and always discarded.
  #
  $FT_CLIENT --port $port ${extraParams[*]} > /tmp/ftClient.$port.log 2>&1 &
  echo $! > "$pidFile"

  if ! corAwaitPort $port 5; then
    echo "ftClientStart: nothing listening on $port; ftClient said:" >&2
    head -5 /tmp/ftClient.$port.log >&2
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
  # So: wait for the SUBACK. ftClient answers 1 on /mqttReady when it has one,
  # and 1 immediately when no --mqttPort was given, so this costs a single poll
  # in the common case.
  #
  case " ${extraParams[*]} " in
    *" --mqttPort "*)
      #
      # 8s, which is longer than it looks: the common case returns on the FIRST
      # poll, and this bound only applies when something is wrong. It has to
      # exceed ftClient's own 5s connect deadline, or the barrier would give up
      # first and report a timeout over the top of the specific reason ftClient
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
        # named $port - the ftClient's HTTP port - while the thing that had failed
        # was the MQTT broker on a different one, and then printed an ftClient log
        # that was empty, so the report carried no information at all.
        #
        local mqttPort=""
        local n
        for ((n = 1; n <= ${#extraParams[@]}; n++)); do
          [ "${extraParams[n-1]}" == "--mqttPort" ] && mqttPort="${extraParams[n]}"
        done

        if [ "$ready" == "-1" ]; then
          echo "ftClientStart: ftClient gave up on the MQTT broker" >&2
        else
          echo "ftClientStart: no MQTT SUBACK within $((deadline / 50))s" >&2
        fi
        echo "  ftClient HTTP port : $port" >&2
        echo "  MQTT broker port   : ${mqttPort:-unknown}" >&2
        echo "  /mqttReady said    : '${ready}'" >&2
        if [ -n "$mqttPort" ] && ! corPortOpen "$mqttPort"; then
          echo "  -> NOTHING is listening on $mqttPort - the MQTT broker never came up," >&2
          echo "     so no deadline here could have helped. Check for a port collision." >&2
        fi
        if [ -s /tmp/ftClient.$port.log ]; then
          echo "  ftClient log:" >&2
          head -5 /tmp/ftClient.$port.log >&2
        else
          echo "  ftClient log /tmp/ftClient.$port.log is empty or absent" >&2
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
# This is also why ftClient's own connect loop cannot cover it: it retries forever,
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


# ftClientUrl <port> <path> - scheme-correct URL for the ftClient on <port>
#
ftClientUrl() {
  local scheme=http
  [ -f "/tmp/ftClient.$1.scheme" ] && scheme=$(cat "/tmp/ftClient.$1.scheme")
  echo "$scheme://localhost:$1$2"
}


# ftClientStop [--port P]
#
# Stops the ftClient on --port (default 7701). Safe to call when not running.
# Kills by port (pkill -f), not by pid file — a prior aborted test run may
# have left an orphan ftClient whose pid file was since cleaned up;
# relying on the pid file would miss it and the new ftClientStart would
# silently fail to bind the port, letting the orphan handle requests
# with the wrong --status. See the distops tests (ftClient status=503
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

  # Port-based kill — matches any ftClient whose cmdline carries
  # "--port <port>". Harmless when no match.
  pkill -f "ftClient.*--port $port( |\$)" 2>/dev/null
  sleep 0.1
  pkill -9 -f "ftClient.*--port $port( |\$)" 2>/dev/null

  \rm -f /tmp/ftClient.$port.pid
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
  local state
  state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)
  [ "$state" != "Z" ]
}


# corValgrindSleep <seconds> - sleep ONLY when running under valgrind (--vt)
#
# Under valgrind the broker runs ~4-5x slower, so an async result (notably a
# notification delivered to ftClient) may not have arrived by the time a test
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

  # A transient empty read (ftClient momentarily unreachable under parallel
  # load) must still be valid JSON — emit "[]" so a downstream `json.load`/`jq`
  # never throws to stderr and flakes the test. For a real count, prefer
  # ftClientCount (reads /count, parser-free).
  [ -z "$raw" ] && raw="[]"

  if [ -n "$KJSON" ] && [ -n "$raw" ] && [ "$raw" != "[]" ]; then
    echo "$raw" | $KJSON -sort | head -c -1
  else
    echo -n "$raw"
  fi
}


# ftClientCount [--port P] - number of requests the mock receiver captured.
#
# Reads ftClient's /count endpoint, which returns a bare integer (never JSON),
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


# ftClientWait <n> [--port P] - block until ftClient has captured at least <n>
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
# reads ftClient's /probeCount (bare integer) so a test can assert the probe fired
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
# "notification landed at ftClient", over 1300 samples on an idle machine, on 32
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
# For HTTPS-notification tests: ftClient serves TLS with this pair and the broker
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
