#
# swTestFunctions.sh - repo-specific test functions for swBroker
#
export SW_BROKER=/home/kz/git/swBroker/swBroker
export SW_BROKER_EXTRA_PARAMS="${SW_BROKER_EXTRA_PARAMS:---database /home/kz/git/swBroker/plugins/swRamDB.so --pretty-print 2 --foreground}"
export SW_DB_NAME="${SW_DB_NAME:-sw}"

# Override swBrokerStart: kargs uses --port (double dash), not -port
swBrokerStart() {
  local port=${SW_BROKER_PORT:-1026}
  swBrokerStop 2>/dev/null
  $SW_BROKER --port $port $SW_BROKER_EXTRA_PARAMS > /dev/null 2>&1 &
  echo $! > ${SW_BROKER_PID_FILE:-/tmp/swBroker.pid}
  swAwaitPort $port 5
}

# Override swDbDrop: drop the entities collection in MongoDB
swDbDrop() {
  local db=${1:-$SW_DB_NAME}

  mongo --quiet --eval 'db.entities.drop()' "$db" > /dev/null 2>&1
}

# Override swDbInit: drop + recreate (for MongoDB, drop is enough)
swDbInit() {
  local db=${1:-$SW_DB_NAME}

  swDbDrop "$db"
}
