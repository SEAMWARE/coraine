#
# swTestFunctions.sh - repo-specific test functions for swBroker
#
export SW_BROKER=/home/kz/git/swBroker/swBroker
export SW_DB_NAME="${SW_DB_NAME:-swTest}"

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
    mongoc) cmd="$cmd --database $SW_BROKER_DIR/plugins/mongoc.so --dbName $SW_ROLE_DB_PREFIX" ;;
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

  if [ -f "$SW_ROLE_PID_FILE" ]; then
    local pid=$(cat "$SW_ROLE_PID_FILE")
    kill $pid 2>/dev/null
    sleep 0.1
    kill -0 $pid 2>/dev/null && kill -9 $pid 2>/dev/null
    \rm -f "$SW_ROLE_PID_FILE"
  fi
}


# -----------------------------------------------------------------------------
#
# swDbDrop [-role <role>] [-tenant <tenant>]
#
# Usage:  swDbDrop
#         swDbDrop -tenant t1
#         swDbDrop -role CP1 -tenant t1
#
swDbDrop() {
  local role="CB"
  local tenant=""

  while [ $# -gt 0 ]; do
    if   [ "$1" == "-role" ];   then role="$2"; shift
    elif [ "$1" == "-tenant" ]; then tenant="$2"; shift
    fi
    shift
  done

  swRoleLookup "$role" || return 1

  case "$SW_DB_TYPE" in
    mongoc)
      local db="$SW_ROLE_DB_PREFIX"
      if [ -n "$tenant" ]; then
        db="${db}-${tenant}"
      fi
      mongo --quiet --eval 'db.entities.drop(); db.subscriptions.drop()' "$db" > /dev/null 2>&1
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
# ftClient - notification receiver for subscription tests
#
FT_CLIENT=$SW_BROKER_DIR/test/funcTests/ftClient/ftClient
FT_CLIENT_PORT=7701
FT_CLIENT_PID_FILE=/tmp/ftClient.pid


# ftClientStart [extra-params...]
#
# Usage:  ftClientStart
#         ftClientStart --port 7702
#
ftClientStart() {
  ftClientStop 2>/dev/null

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

  $FT_CLIENT --port $port ${extraParams[*]} > /dev/null 2>&1 &
  echo $! > "$FT_CLIENT_PID_FILE"
  swAwaitPort $port 5
}


# ftClientStop
#
ftClientStop() {
  if [ -f "$FT_CLIENT_PID_FILE" ]; then
    local pid=$(cat "$FT_CLIENT_PID_FILE")
    kill $pid 2>/dev/null
    sleep 0.1
    kill -0 $pid 2>/dev/null && kill -9 $pid 2>/dev/null
    \rm -f "$FT_CLIENT_PID_FILE"
  fi
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
