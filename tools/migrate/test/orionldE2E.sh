#!/bin/bash
#
# FILE            orionldE2E.sh
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# The whole migration, end to end, on a made-up Orion-LD deployment:
#
#   1. fabricate an Orion-LD database: MongoDB 'migtestorion' + 'migtestorion-t1' (current state),
#      PostgreSQL 'migtestorion' + 'migtestorion_t1' (history)        - orionldFabricate.{js,sql}
#   2. read it with orionldExport.py, and compare the stream with the one the broker's functest
#      imports (test/funcTests/fixtures/migrate/orionld-stream.ndjson) - the reader's test
#   3. import the stream, twice: into corDB + timescale and into mongoc + timescale
#   4. start coraine on each migrated store and GET everything back through the API; the two must
#      answer the same, and as orionldE2E.expected says
#
# Needs: a MongoDB and a PostgreSQL with PostGIS (the ones the functests use), mongosh, psql, and
# a python with pymongo and psycopg (3 or 2) - PYTHON=/path/to/python if it is not python3.
#
# Every database it creates is named migtest*, and only those are dropped. --keep leaves them.
#
# Usage: orionldE2E.sh [--keep] [--update]   (--update rewrites the fixture and the expected output)
#
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
PYTHON=${PYTHON:-python3}
BROKER=${BROKER_BIN:-coraine}
PORT=${MIGTEST_PORT:-19830}
PGHOST=${COR_TROE_HOST:-localhost}
PGUSER=${COR_TROE_USER:-postgres}
MONGO=${COR_MONGO_HOST:-localhost}
FIXTURE=$ROOT/test/funcTests/fixtures/migrate/orionld-stream.ndjson
EXPECTED=$HERE/orionldE2E.expected
W=$(mktemp -d /tmp/migtest-e2e.XXXXXX)
KEEP=0
UPDATE=0

for a in "$@"; do
  case "$a" in
    --keep)   KEEP=1 ;;
    --update) UPDATE=1 ;;
    *)        echo "usage: $0 [--keep] [--update]"; exit 1 ;;
  esac
done

PGPWD=""
[ -n "$PGPASSWORD" ] && PGPWD="--troePwd $PGPASSWORD"

psqlDo() { psql -q -h "$PGHOST" -U "$PGUSER" -v ON_ERROR_STOP=1 "$@"; }

dropAll() {
  mongosh --quiet --host "$MONGO" --eval '
    for (const d of db.adminCommand({ listDatabases: 1 }).databases)
      if (d.name.startsWith("migtest")) db.getSiblingDB(d.name).dropDatabase();' > /dev/null
  for d in $(psql -h "$PGHOST" -U "$PGUSER" -tAc "SELECT datname FROM pg_database WHERE datname LIKE 'migtest%'" postgres); do
    psqlDo -c "DROP DATABASE IF EXISTS \"$d\"" postgres
  done
}

cleanup() {
  [ -f "$W/broker.pid" ] && kill "$(cat "$W/broker.pid")" 2>/dev/null
  [ $KEEP == 1 ] && { echo "kept: the migtest* databases and $W"; return; }
  dropAll
  rm -rf "$W"
}
trap cleanup EXIT

dropAll   # what a --keep run left: an import goes into EMPTY stores

fail() { echo "FAIL: $*"; exit 1; }


echo "1. fabricate the Orion-LD database"
mongosh --quiet --host "$MONGO" "$HERE/orionldFabricate.js" || fail "mongosh"
for t in default t1; do
  db=migtestorion; [ $t != default ] && db=migtestorion_$t
  psqlDo -c "DROP DATABASE IF EXISTS $db" -c "CREATE DATABASE $db" postgres || fail "create $db"
  psqlDo -d $db -v tenant=$t -f "$HERE/orionldFabricate.sql" > /dev/null || fail "fabricate $db"
done


echo "2. read it (orionldExport.py)"
CONN="host=$PGHOST user=$PGUSER"
[ -n "$PGPASSWORD" ] && CONN="$CONN password=$PGPASSWORD"
$PYTHON "$ROOT/tools/migrate/orionldExport.py" --mongo "mongodb://$MONGO:27017" --db migtestorion --troe "$CONN" --out "$W/stream.ndjson" || fail "orionldExport.py"

if [ $UPDATE == 1 ]; then
  mkdir -p "$(dirname "$FIXTURE")"
  tail -n +2 "$W/stream.ndjson" > "$FIXTURE"
  echo "   fixture rewritten: $FIXTURE"
fi
diff <(tail -n +2 "$W/stream.ndjson") "$FIXTURE" > "$W/stream.diff" || { cat "$W/stream.diff"; fail "the stream differs from $FIXTURE"; }
echo "   the stream is the fixture the functest imports ($(wc -l < "$FIXTURE") records)"


echo "3/4. import it - corDB and mongoc, both with timescale - and read it back"
for store in corDB mongoc; do
  case $store in
    corDB)  DBARGS="-db corDB --dbDir $W/cordb" ;;
    mongoc) DBARGS="-db mongoc --dbHost $MONGO --dbName migtest-cor --globalDb migtest-coraine" ;;
  esac
  TROEARGS="--troe timescale --troeHost $PGHOST --troeUser $PGUSER $PGPWD --troeName migtest_$store"

  $BROKER -fg -p $PORT $DBARGS $TROEARGS --importFile "$W/stream.ndjson" > "$W/import-$store.out" 2>&1 || { cat "$W/import-$store.out"; fail "import into $store"; }
  sed 's/^/   /' "$W/import-$store.out"

  $BROKER -fg -p $PORT -pp 2 $DBARGS $TROEARGS > "$W/broker-$store.log" 2>&1 &
  echo $! > "$W/broker.pid"
  for i in $(seq 1 50); do curl -s -o /dev/null "http://localhost:$PORT/ngsi-ld/v1/types" && break; sleep 0.1; done

  get() { echo "== GET $1 ${2:+($2)}"; curl -s ${2:+-H "NGSILD-Tenant: $2"} "http://localhost:$PORT$1"; echo; }
  {
    get '/ngsi-ld/v1/entities?type=Vehicle,https://example.org/Parking&options=sysAttrs'
    get '/ngsi-ld/v1/entities?type=https://example.org/Room&options=sysAttrs' t1
    get '/ngsi-ld/v1/subscriptions?options=sysAttrs'
    get '/ngsi-ld/v1/subscriptions?options=sysAttrs' t1
    get '/ngsi-ld/v1/csourceRegistrations/urn:ngsi-ld:ContextSourceRegistration:R1?options=sysAttrs'
    get '/ngsi-ld/v1/temporal/entities/urn:ngsi-ld:Vehicle:V1?options=sysAttrs'
    get '/ngsi-ld/v1/temporal/entities/urn:ngsi-ld:Vehicle:V9?options=sysAttrs'
    get '/ngsi-ld/v1/temporal/entities/urn:ngsi-ld:Room:R1?options=sysAttrs' t1
  } > "$W/get-$store.out"

  kill "$(cat "$W/broker.pid")"; wait "$(cat "$W/broker.pid")" 2>/dev/null; rm -f "$W/broker.pid"
done

diff "$W/get-corDB.out" "$W/get-mongoc.out" > "$W/stores.diff" || { cat "$W/stores.diff"; fail "corDB and mongoc answer differently"; }
echo "   corDB and mongoc answer the same"

[ $UPDATE == 1 ] && cp "$W/get-corDB.out" "$EXPECTED" && echo "   expected output rewritten: $EXPECTED"
diff "$W/get-corDB.out" "$EXPECTED" > "$W/expected.diff" || { cat "$W/expected.diff"; fail "the migrated data differs from $EXPECTED"; }
echo "   and as $(basename "$EXPECTED") says"

echo "PASS"
