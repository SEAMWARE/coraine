#
# swTestFunctions.sh - repo-specific test functions for swBroker
#
export SW_BROKER=/home/kz/git/swBroker/swBroker
export SW_DB_NAME="${SW_DB_NAME:-swTest}"
SW_MONGO_PORT=${SW_MONGO_PORT:-27018}

SW_BROKER_DIR=$(dirname $SW_BROKER)


# -----------------------------------------------------------------------------
#
# Role definitions: port, pidFile, dbPrefix
#
#   role      port   pidFile                  dbPrefix
#
SW_ROLES="
   CB        1026   /tmp/swBroker_CB.pid      swTest
   CP1       1027   /tmp/swBroker_CP1.pid     swTest_cp1
   CP2       1028   /tmp/swBroker_CP2.pid     swTest_cp2
   CP3       1029   /tmp/swBroker_CP3.pid     swTest_cp3
   CP4       1030   /tmp/swBroker_CP4.pid     swTest_cp4
   CP5       1031   /tmp/swBroker_CP5.pid     swTest_cp5
"

CB_PORT=1026
CP1_PORT=1027
CP2_PORT=1028
CP3_PORT=1029
CP4_PORT=1030
CP5_PORT=1031

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

  local cmd="$SW_BROKER --port $SW_ROLE_PORT --pretty-print 2 --foreground"

  # Current-state DB plugin
  case "$SW_DB_TYPE" in
    mongoc) cmd="$cmd --database $SW_BROKER_DIR/plugins/mongoc.so --dbName $SW_ROLE_DB_PREFIX --dbPort $SW_MONGO_PORT" ;;
    ramdb)  cmd="$cmd --database $SW_BROKER_DIR/plugins/swRamDB.so" ;;
    NONE)   ;;  # compiled-in default
    *)      echo "swBrokerStart: unknown -db type: $SW_DB_TYPE"; return 1 ;;
  esac

  # TRoE DB plugin (future)
  case "$SW_TROE_DB_TYPE" in
    NONE|"") ;;  # compiled-in default or unset
    *)       echo "swBrokerStart: unknown -troeDb type: $SW_TROE_DB_TYPE"; return 1 ;;
  esac

  # Append test-specific extra params
  if [ ${#extraParams[@]} -gt 0 ]; then
    cmd="$cmd ${extraParams[*]}"
  fi

  $cmd > /dev/null 2>&1 &
  echo $! > "$SW_ROLE_PID_FILE"
  swAwaitPort $SW_ROLE_PORT 10
}


# -----------------------------------------------------------------------------
#
# swBrokerStop [-role <role>]
#
swBrokerStop() {
  local role="CB"

  if [ "$1" == "-role" ]; then role="$2"; fi

  swRoleLookup "$role" || return 1

  # Port-based kill so orphans from aborted prior runs (with no live pid
  # file) are still caught. Matches any swBroker whose cmdline carries
  # "--port <port>".
  pkill -f "swBroker.*--port $SW_ROLE_PORT( |\$)" 2>/dev/null
  sleep 0.1
  pkill -9 -f "swBroker.*--port $SW_ROLE_PORT( |\$)" 2>/dev/null

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
          mongosh --port $SW_MONGO_PORT --quiet --eval 'db.entities.drop(); db.subscriptions.drop(); db.registrations.drop()' "$db" > /dev/null 2>&1
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
# ftClient - generic mock endpoint for forward-target / notification-receiver tests
#
# Each instance has its own PID file keyed by port, so multiple ftClients
# can run concurrently (one per CSR target) and be stopped individually.
#
FT_CLIENT=$SW_BROKER_DIR/test/funcTests/ftClient/ftClient
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

  $FT_CLIENT --port $port ${extraParams[*]} > /dev/null 2>&1 &
  echo $! > "$pidFile"
  swAwaitPort $port 5
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


# ftClientDump - retrieve accumulated notifications
#
ftClientDump() {
  local raw
  raw=$(curl -s http://localhost:$FT_CLIENT_PORT/dump)

  if [ -n "$KJSON" ] && [ -n "$raw" ] && [ "$raw" != "[]" ]; then
    echo "$raw" | $KJSON -sort | head -c -1
  else
    echo -n "$raw"
  fi
}


# ftClientReset - clear accumulated notifications
#
ftClientReset() {
  curl -s -X DELETE http://localhost:$FT_CLIENT_PORT/dump > /dev/null
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

  local running=$(docker ps --filter name=context-server -q 2>/dev/null)
  if [ -z "$running" ]; then
    docker run --rm -d --name context-server -p $CONTEXT_SERVER_PORT:8080 -e MEMORY_ENABLED=true wistefan/context-server > /dev/null 2>&1
    sleep 3  # context server is slow to start
  fi
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
