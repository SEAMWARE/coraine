#
# swTestFunctions.sh - repo-specific test functions for swBroker
#
export SW_BROKER="${SW_BROKER:-swBroker}"        # broker from PATH (installed via make di)
export SW_DB_NAME="${SW_DB_NAME:-swTest}"
SW_MONGO_PORT=${SW_MONGO_PORT:-27017}
SW_TROE_PORT=${SW_TROE_PORT:-5432}               # timescale/postgres port
SW_TROE_USER=${SW_TROE_USER:-postgres}           # timescale/postgres user

# Plugins from their install site; ftClient from the repo (cmake builds it there).
SW_PLUGIN_DIR="${SW_PLUGIN_DIR:-/opt/seamware/plugins}"
SW_REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"


# -----------------------------------------------------------------------------
#
# Role definitions: port, pidFile, dbPrefix
#
#   role      port   pidFile                  dbPrefix
#
# Roles: CB = main broker; CB2-5 = secondary main brokers (federation /
# replication); CP1-5 = brokers acting as context providers. Ports stay below
# 1036 (reserved for the parallel ETSI run — see ~/bin/gateAll).
SW_ROLES="
   CB        1026   /tmp/swBroker_CB.pid      swTest
   CP1       1027   /tmp/swBroker_CP1.pid     swTest_cp1
   CP2       1028   /tmp/swBroker_CP2.pid     swTest_cp2
   CP3       1029   /tmp/swBroker_CP3.pid     swTest_cp3
   CP4       1030   /tmp/swBroker_CP4.pid     swTest_cp4
   CP5       1031   /tmp/swBroker_CP5.pid     swTest_cp5
   CB2       1032   /tmp/swBroker_CB2.pid     swTest_cb2
   CB3       1033   /tmp/swBroker_CB3.pid     swTest_cb3
   CB4       1034   /tmp/swBroker_CB4.pid     swTest_cb4
   CB5       1035   /tmp/swBroker_CB5.pid     swTest_cb5
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

# swRoleLookup - resolve role to port/pidFile/dbPrefix
# Sets: SW_ROLE_PORT, SW_ROLE_PID_FILE, SW_ROLE_DB_PREFIX
swRoleLookup() {
  local role="$1"
  local line

  line=$(echo "$SW_ROLES" | awk -v r="$role" '$1 == r { print $2, $3, $4 }')
  if [ -z "$line" ]; then
    echo "swRoleLookup: unknown role: $role"
    return 1
  fi

  SW_ROLE_PORT=$(echo "$line" | awk '{print $1}')
  SW_ROLE_PID_FILE=$(echo "$line" | awk '{print $2}')
  SW_ROLE_DB_PREFIX=$(echo "$line" | awk '{print $3}')
}


# -----------------------------------------------------------------------------
#
# swBrokerStart [-role <role>] [extra-broker-params...]
#
# Usage:  swBrokerStart
#         swBrokerStart -role CP1
#         swBrokerStart -role CP1 -distOps
#
swBrokerStart() {
  local role="CB"
  local -a extraParams

  while [ $# -gt 0 ]; do
    if [ "$1" == "-role" ]; then role="$2"; shift
    else extraParams+=("$1")
    fi
    shift
  done

  swRoleLookup "$role" || return 1
  swBrokerStop -role "$role" 2>/dev/null

  # --httpEndpoint is pinned to localhost so served-@context URLs, distributed-sub
  # callbacks and forwarded Link headers are host-independent (the broker now
  # auto-detects a LAN IP by default, which would make expected outputs vary per
  # test machine). Tests that need a different endpoint append their own -he.
  local cmd="$SW_BROKER --port $SW_ROLE_PORT --pretty-print 2 --foreground --httpEndpoint http://localhost:$SW_ROLE_PORT"

  # Current-state DB plugin
  case "$SW_DB_TYPE" in
    mongoc) cmd="$cmd --database $SW_PLUGIN_DIR/db/currentState/mongoc.so --dbName $SW_ROLE_DB_PREFIX --dbPort $SW_MONGO_PORT" ;;
    ramdb)  cmd="$cmd --database $SW_PLUGIN_DIR/db/currentState/swRamDB.so" ;;
    NONE)   ;;  # compiled-in default
    *)      echo "swBrokerStart: unknown -db type: $SW_DB_TYPE"; return 1 ;;
  esac

  # TRoE DB plugin (future)
  case "$SW_TROE_DB_TYPE" in
    NONE|"") ;;  # compiled-in default or unset
    *)       echo "swBrokerStart: unknown -troeDb type: $SW_TROE_DB_TYPE"; return 1 ;;
  esac

  # Timescale TRoE convenience: when a test asks for "--troe timescale" without
  # naming the DB, derive the role-keyed name (corh_<role>) — the same name
  # swTroeInit/swTroeDrop create/drop — and add --troeUser. Tests that pass an
  # explicit --troeName keep full control.
  if printf '%s\n' "${extraParams[@]}" | grep -qx 'timescale' && \
     ! printf '%s\n' "${extraParams[@]}" | grep -qx -- '--troeName'; then
    extraParams+=(--troeName "$(swTroeDbName "$role")" --troeUser "$SW_TROE_USER")
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
  if [ "$SW_VALGRIND" == "1" ] && [ "$role" == "CB" ]; then
    local vgLog="${SW_VALGRIND_LOG:-/tmp/swValgrind}"
    # errors-for-leak-kinds=none: leaks must NOT inflate "ERROR SUMMARY", so that
    # line stays a pure memory-error count (Invalid read/write, uninitialised, …).
    # The engine fails on leaks by reading the LEAK SUMMARY lost-byte counts
    # directly, so leaks don't need to count as "errors" to be caught — and this
    # keeps the engine's E (errors) and L (leaks) tallies cleanly separated.
    local vg="valgrind --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=none --track-origins=yes --num-callers=40 --child-silent-after-fork=yes"
    if [ -f "test/funcTests/valgrind.supp" ]; then
      vg="$vg --suppressions=test/funcTests/valgrind.supp"
    fi
    vg="$vg --log-file=${vgLog}.%p.vg"
    cmd="$vg $cmd"
    awaitSecs=90   # valgrind makes startup ~20x slower
  fi

  $cmd > "/tmp/swBroker.${role}.log" 2>&1 &
  echo $! > "$SW_ROLE_PID_FILE"
  swAwaitPort $SW_ROLE_PORT $awaitSecs
}


# -----------------------------------------------------------------------------
#
# swBrokerStop [-role <role>] [-all]
#
# No argument stops the CB, as it always has. -all stops every role in SW_ROLES,
# for teardowns that would otherwise have to name each secondary they started -
# forgetting one leaks a broker onto its port, which is the orphan flakiness in
# swTestFunctions' own stop helpers.
#
swBrokerStop() {
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
    for r in $(echo "$SW_ROLES" | awk '{print $1}'); do
      swBrokerStop -role "$r"
    done
    return 0
  fi

  swRoleLookup "$role" || return 1

  # Under valgrind (--vt), the CB must be stopped GRACEFULLY: SIGTERM lets
  # onSignal()->exit(0) run, which is what makes valgrind write its leak
  # report. A quick SIGKILL would truncate it. Wait (bounded) for the valgrind
  # process to actually exit before returning.
  if [ "$SW_VALGRIND" == "1" ] && [ "$role" == "CB" ] && [ -f "$SW_ROLE_PID_FILE" ]; then
    local pid; pid=$(cat "$SW_ROLE_PID_FILE")
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null
      local n=0
      while kill -0 "$pid" 2>/dev/null && [ $n -lt 1200 ]; do sleep 0.1; n=$((n + 1)); done
      kill -9 "$pid" 2>/dev/null   # backstop only if the wait timed out
    fi
    \rm -f "$SW_ROLE_PID_FILE"
    return
  fi

  # Port-based kill so orphans from aborted prior runs (with no live pid
  # file) are still caught. Matches any swBroker whose cmdline carries
  # "--port <port>".
  local pat="swBroker.*--port $SW_ROLE_PORT( |\$)"
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

  \rm -f "$SW_ROLE_PID_FILE"
}


# -----------------------------------------------------------------------------
#
# swDbDrop [-role <role>] [-tenant <tenant>] [-db <dbName>]
#
# Usage:  swDbDrop                    # drop collections in CB's default db
#         swDbDrop -tenant t1         # drop collections in CB's tenant db
#         swDbDrop -role CP1          # drop collections in CP1's db
#         swDbDrop -db swBroker       # drop the entire "swBroker" database
#
swDbDrop() {
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

  case "$SW_DB_TYPE" in
    mongoc)
      if [ -n "$explicitDb" ]; then
        mongosh --port $SW_MONGO_PORT --quiet --eval 'db.dropDatabase()' "$explicitDb" > /dev/null 2>&1
      else
        swRoleLookup "$role" || return 1
        local db="$SW_ROLE_DB_PREFIX"
        if [ -n "$tenant" ]; then
          db="${db}-${tenant}"
          mongosh --port $SW_MONGO_PORT --quiet --eval 'db.entities.drop(); db.subscriptions.drop(); db.registrations.drop(); db.snapshots.drop()' "$db" > /dev/null 2>&1
        else
          # No tenant specified → drop default + all tenant-suffixed dbs.
          # Tests that leave tenant state behind shouldn't bleed into later
          # tests that assume ngsild_tenants_total == 1.
          local prefix="$SW_ROLE_DB_PREFIX"
          mongosh --port $SW_MONGO_PORT --quiet --eval \
            "db.adminCommand('listDatabases').databases \
              .map(d=>d.name) \
              .filter(n=>n===\"$prefix\"||n.startsWith(\"$prefix-\")) \
              .forEach(n=>db.getSiblingDB(n).dropDatabase())" > /dev/null 2>&1
        fi
      fi
      ;;
    ramdb|NONE)
      # No-op: broker restart clears the RAM store
      ;;
  esac
}

# swDbInit: drop + recreate
swDbInit() {
  swDbDrop "$@"
}


# -----------------------------------------------------------------------------
#
# TRoE (timescale/postgres) database helpers — the postgres counterpart of
# swDbDrop/swDbInit. Role-keyed like the mongo helpers: the TRoE DB for a role
# is "corh_<role>" (lowercased), so CB -> corh_cb, CP1 -> corh_cp1.
# swBrokerStart derives the same name for "--troe timescale".
#
#   swTroeDbName [role]            # echo the derived DB name (default CB)
#   swTroeInit  [-role R] [-db N]  # DROP + CREATE the TRoE DB
#   swTroeDrop  [-role R] [-db N]  # DROP the TRoE DB (and its snapshot children)
#
swTroeDbName() {
  local role="${1:-CB}"
  echo "corh_${role,,}"
}

swTroeInit() {
  local role="CB" db=""
  while [ $# -gt 0 ]; do
    if   [ "$1" == "-role" ]; then role="$2"; shift
    elif [ "$1" == "-db" ];   then db="$2";   shift
    fi
    shift
  done
  [ -z "$db" ] && db="$(swTroeDbName "$role")"

  # Drop any per-tenant / per-snapshot child databases ("<db>_<suffix>") left
  # by a previous run before recreating the base — each tenant now owns its own
  # physical database, so stale children would otherwise leak across runs.
  psql -h localhost -p "$SW_TROE_PORT" -U "$SW_TROE_USER" -tAc \
    "SELECT datname FROM pg_database WHERE datname LIKE '${db}_%'" 2>/dev/null | \
    while read -r child; do
      [ -n "$child" ] && psql -h localhost -p "$SW_TROE_PORT" -U "$SW_TROE_USER" -c "DROP DATABASE IF EXISTS \"$child\"" >/dev/null 2>&1
    done

  psql -h localhost -p "$SW_TROE_PORT" -U "$SW_TROE_USER" -c "DROP DATABASE IF EXISTS $db" >/dev/null 2>&1
  psql -h localhost -p "$SW_TROE_PORT" -U "$SW_TROE_USER" -c "CREATE DATABASE $db"          >/dev/null
}

swTroeDrop() {
  local role="CB" db=""
  while [ $# -gt 0 ]; do
    if   [ "$1" == "-role" ]; then role="$2"; shift
    elif [ "$1" == "-db" ];   then db="$2";   shift
    fi
    shift
  done
  [ -z "$db" ] && db="$(swTroeDbName "$role")"

  # Drop per-tenant / per-snapshot child TRoE DBs first ("<db>_<suffix>", e.g.
  # "<db>_t1" or "<db>_snap_<hex>"), then the base. Each tenant now owns its own
  # physical database; a child with the base as a prefix would otherwise leak
  # across runs.
  psql -h localhost -p "$SW_TROE_PORT" -U "$SW_TROE_USER" -tAc \
    "SELECT datname FROM pg_database WHERE datname LIKE '${db}_%'" 2>/dev/null | \
    while read -r child; do
      [ -n "$child" ] && psql -h localhost -p "$SW_TROE_PORT" -U "$SW_TROE_USER" -c "DROP DATABASE IF EXISTS \"$child\"" >/dev/null 2>&1
    done
  psql -h localhost -p "$SW_TROE_PORT" -U "$SW_TROE_USER" -c "DROP DATABASE IF EXISTS $db" >/dev/null 2>&1
}

# Default-role (CB) TRoE DB name, for tests that inspect the TRoE tables
# directly with `psql -d "$SW_TROE_DB"`.
export SW_TROE_DB="$(swTroeDbName CB)"


# -----------------------------------------------------------------------------
#
# swSnapDrop [-role <role>]
#
# Drop every snapshot-tenant DB belonging to <role> (default: CB). Snap
# tenants are named "${prefix}-${role}-_snap_<hex>" by snapshotTenantCreate;
# this enumerates them via listDatabases and dropDatabase()s each.
#
# swDbDrop already enumerates ${prefix}-* (so it incidentally cleans
# snap-tenants too), but swSnapDrop is the explicit, surgical helper for
# tests that want to assert "snapshots cleaned, nothing else touched".
# Recommended: call from snapshot tests' INIT (clean leftovers) and
# TEARDOWN (clean what this test created).
#
swSnapDrop() {
  local role="CB"

  while [ $# -gt 0 ]; do
    if [ "$1" == "-role" ]; then role="$2"; shift; fi
    shift
  done

  case "$SW_DB_TYPE" in
    mongoc)
      swRoleLookup "$role" || return 1
      local rolePrefix="${SW_ROLE_DB_PREFIX}-"
      mongosh --port $SW_MONGO_PORT --quiet --eval \
        "db.adminCommand('listDatabases').databases \
          .map(d=>d.name) \
          .filter(n=>n.startsWith(\"$rolePrefix\")&&n.includes(\"-_snap_\")) \
          .forEach(n=>db.getSiblingDB(n).dropDatabase())" > /dev/null 2>&1
      ;;
    ramdb|NONE)
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
FT_CLIENT=$SW_REPO_DIR/test/funcTests/ftClient/ftClient
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

  # Record the scheme so ftClientDump/ftClientReset reach the right URL: a
  # --httpsKey/--httpsCertificate ftClient serves HTTPS, plain HTTP otherwise.
  local scheme=http
  case " ${extraParams[*]} " in
    *" --httpsKey "*|*" -k "*) scheme=https ;;
  esac
  echo "$scheme" > /tmp/ftClient.$port.scheme

  $FT_CLIENT --port $port ${extraParams[*]} > /dev/null 2>&1 &
  echo $! > "$pidFile"
  swAwaitPort $port 5
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
# swValgrindSleep <seconds> - sleep ONLY when running under valgrind (--vt)
#
# Under valgrind the broker runs ~4-5x slower, so an async result (notably a
# notification delivered to ftClient) may not have arrived by the time a test
# reads for it. This adds a settle delay on the valgrind path only; a normal run
# is unaffected and stays fast. Always returns 0.
#
swValgrindSleep() {
  [ "$SW_VALGRIND" == "1" ] && sleep "$1"
  return 0
}


ftClientDump() {
  # Let any in-flight notification land before reading (valgrind path only).
  # Also makes negative checks ("should NOT notify") robust: a late notification
  # would have arrived during the settle, so an empty dump is trustworthy.
  swValgrindSleep "${SW_VALGRIND_DUMP_SETTLE:-1.5}"

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
  swValgrindSleep "${SW_VALGRIND_DUMP_SETTLE:-1.5}"

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


# ftClientReset [--port P] - clear accumulated notifications
#
ftClientReset() {
  local port=$FT_CLIENT_PORT
  while [ $# -gt 0 ]; do
    if [ "$1" == "--port" ] || [ "$1" == "-p" ]; then
      port="$2"
      shift
    fi
    shift
  done
  curl -sk -X DELETE "$(ftClientUrl $port /dump)" > /dev/null
}


# swHttpsCertGen [keyFile] [certFile] - generate a self-signed key + certificate
#
# For HTTPS-notification tests: ftClient serves TLS with this pair and the broker
# (started with --insecureNotif) accepts the self-signed cert. Defaults to
# /tmp/swFtClient.key + /tmp/swFtClient.pem, CN=localhost. All openssl chatter
# goes to /dev/null so INIT stays stderr-clean.
#
swHttpsCertGen() {
  local keyFile=${1:-/tmp/swFtClient.key}
  local certFile=${2:-/tmp/swFtClient.pem}

  openssl genrsa -out "$keyFile" 2048 > /dev/null 2>&1
  openssl req -days 365 -new -x509 -key "$keyFile" -out "$certFile" \
          -subj "/C=ES/ST=Madrid/L=Madrid/O=Seamware/OU=test/CN=localhost/" > /dev/null 2>&1
}


# -----------------------------------------------------------------------------
#
# Context Server (wistefan/context-server on port 7080)
#
CONTEXT_SERVER_PORT=7080


# contextServerStart - ensure the Docker context server is running
#
contextServerStart() {
  local dockerExec=$(which docker 2>/dev/null)
  if [ -z "$dockerExec" ]; then
    echo "contextServerStart: docker not found"
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


# contextServerStop - stop the Docker context server
#
contextServerStop() {
  docker kill context-server > /dev/null 2>&1
}


# contextServerPush - push a JSON-LD context to the context server
#
# Usage: contextServerPush <url-path> '<json-ld-context>'
#
contextServerPush() {
  local urlPath="$1"
  local payload="$2"

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
