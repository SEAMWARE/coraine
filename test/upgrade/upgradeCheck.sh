#!/bin/bash
#
# FILE            upgradeCheck.sh
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# upgradeCheck.sh - the data an older release wrote, opened by this one (and back again)
#
#   test/upgrade/upgradeCheck.sh [--from <image>] [--to <coraine>] [--store mongoc|corDB|all] [--keep]
#
#   --from    the older release, as its image (default quay.io/seamware/coraine:0.4.0), run with
#             --network host
#   --to      the broker under test (default: coraine on PATH); its plugins are found the way it
#             always finds them - SEAMWARE_PLUGIN_DIR, else /opt/seamware/plugins
#   --store   mongoc (the local MongoDB + the local postgres for the TRoE), corDB (a store directory,
#             history inside it - only where --from has persistence: a corDB without --dbDir keeps
#             nothing to upgrade), or all (default)
#   --keep    leave the databases / directory and the logs (default: dropped)
#
# Per store, five steps:
#
#   1. --from writes: entities of several types (multi-attribute + datasetId, Point and Polygon,
#      languageMap, sub-attributes, JsonProperty, Relationship, scope, multi-type), one in a tenant,
#      subscriptions (one paused), registrations, and history (creates + PATCHes with observedAt)
#   2. --from reads it all back: "before"
#   3. --to on the same data reads it all back: "after" - must equal "before" (sysAttrs included), and
#      on mongoc so must the stored documents (a store that rewrites what it opens shows there). A read
#      that differs between two releases on purpose is listed in KNOWN below - shown, not counted
#   4. --to: the queries (type, q, geo, scopeQ), the mongoc indexes ({type,createdAt,_id} there, the old
#      type_1 gone), no E/X lines in its log, a PATCH that a subscription notifies, the history readable
#   5. --from again on what --to has touched - the downgrade: reads compared with --to's last
#
# The postgres password, where one is needed: PGPASSWORD (libpq reads it; the image gets it passed).
#
# Exit code: the number of failed checks (0 = upgrade clean). A downgrade difference is reported and
# counted separately (DOWNGRADE lines) - it does not fail the run: going back is not promised.
#
FROM=quay.io/seamware/coraine:0.4.0
TO=coraine
STORES=all
KEEP=0
PORT=${UPG_PORT:-19046}           # the broker, old and new
RPORT=${UPG_RPORT:-19047}         # the notification receiver
MONGO_HOST=${COR_MONGO_HOST:-localhost}
MONGO_PORT=${COR_MONGO_PORT:-27017}
PG_HOST=${COR_TROE_HOST:-localhost}
PG_PORT=${COR_TROE_PORT:-5432}
PG_USER=${COR_TROE_USER:-postgres}
DB=upgradeCheck                   # mongoc dbName
PGDB=upgradecheck                 # TRoE database
TENANT=upgt

while [ $# -gt 0 ]; do
  case "$1" in
    --from)  FROM=$2; shift ;;
    --to)    TO=$2; shift ;;
    --store) STORES=$2; shift ;;
    --keep)  KEEP=1 ;;
    -u|--help) sed -n '10,40p' "$0"; exit 0 ;;
    *) echo "upgradeCheck: unknown option $1"; exit 1 ;;
  esac
  shift
done

WORK=$(mktemp -d /tmp/upgradeCheck.XXXXXX)
BASE=http://localhost:$PORT/ngsi-ld/v1
CT='Content-Type: application/json'
FAILS=0
DOWNGRADE_DIFFS=0
OLD_CONTAINER=upgradeCheck-old-$$
RECEIVER_PID=""

say()  { echo "== $*"; }
ok()   { echo "   ok    $*"; }
bad()  { echo "   FAIL  $*"; FAILS=$((FAILS + 1)); }

#
# The broker, either release - started in the foreground, its log in $WORK/<phase>.log
#
oldStart()   # oldStart <phase> <store args...>
{
  local phase=$1; shift
  docker run -d --rm --name $OLD_CONTAINER --network host --user "$(id -u):$(id -g)" -v "$WORK:$WORK" -e PGPASSWORD \
    --entrypoint coraine "$FROM" --foreground --port $PORT --httpEndpoint http://localhost:$PORT "$@" > /dev/null || return 1
  ( docker logs -f $OLD_CONTAINER > "$WORK/$phase.log" 2>&1 & )
  awaitBroker
}

oldStop()
{
  docker stop -t 10 $OLD_CONTAINER > /dev/null 2>&1
  for i in $(seq 1 50); do docker inspect $OLD_CONTAINER > /dev/null 2>&1 || return 0; sleep 0.2; done
}

NEW_PID=""
newStart()   # newStart <phase> <store args...>
{
  local phase=$1; shift
  $TO --foreground --port $PORT --httpEndpoint http://localhost:$PORT "$@" > "$WORK/$phase.log" 2>&1 &
  NEW_PID=$!
  awaitBroker
}

newStop()
{
  [ -z "$NEW_PID" ] && return
  kill -TERM $NEW_PID 2>/dev/null
  for i in $(seq 1 100); do kill -0 $NEW_PID 2>/dev/null || break; sleep 0.1; done
  kill -9 $NEW_PID 2>/dev/null
  wait $NEW_PID 2>/dev/null
  NEW_PID=""
}

awaitBroker()
{
  for i in $(seq 1 300); do
    curl -s -o /dev/null http://localhost:$PORT/version && return 0
    sleep 0.1
  done
  echo "upgradeCheck: no broker on port $PORT"
  return 1
}

#
# req <expected-status> <method> <path> [body] [extra curl args...] - a request whose status is checked
#
req()
{
  local expect=$1 method=$2 path=$3 body=$4; shift 4
  local args=(-s -o "$WORK/last.body" -w '%{http_code}' -X "$method" "$BASE$path")
  [ -n "$body" ] && args+=(-H "$CT" -d "$body")
  local status
  status=$(curl "${args[@]}" "$@")
  if [ "$status" != "$expect" ]; then
    bad "$method $path: $status (expected $expect): $(head -c 400 "$WORK/last.body")"
    return 1
  fi
  return 0
}

#
# The data
#
T0=2026-01-10T10:00:00.000Z
T1=2026-01-10T11:00:00.000Z
T2=2026-01-10T12:00:00.000Z
T3=2026-01-10T13:00:00.000Z

ENTITY_IDS="urn:ngsi-ld:Vehicle:upg1 urn:ngsi-ld:Vehicle:upg2 urn:ngsi-ld:Vehicle:upg3 urn:ngsi-ld:Vehicle:upg4 urn:ngsi-ld:Vehicle:upg5 urn:ngsi-ld:Building:upg1 urn:ngsi-ld:Sensor:upg1"

write()
{
  say "writing with $FROM"
  req 201 POST /entities '{
    "id": "urn:ngsi-ld:Vehicle:upg1", "type": "Vehicle", "scope": "/area/north",
    "speed": [
      { "type": "Property", "value": 80, "unitCode": "KMH", "observedAt": "'$T0'" },
      { "type": "Property", "value": 82, "unitCode": "KMH", "observedAt": "'$T0'", "datasetId": "urn:ngsi-ld:Dataset:gps" }
    ],
    "location": { "type": "GeoProperty", "value": { "type": "Point", "coordinates": [ 13.35, 52.51 ] } },
    "name": { "type": "LanguageProperty", "languageMap": { "en": "Truck one", "sv": "Lastbil ett" } },
    "owner": { "type": "Relationship", "object": "urn:ngsi-ld:Person:p1", "since": { "type": "Property", "value": { "@type": "DateTime", "@value": "2025-05-01T00:00:00.000Z" } } },
    "brand": { "type": "Property", "value": "Volvo", "model": { "type": "Property", "value": "FH16", "year": { "type": "Property", "value": 2024 } } },
    "meta": { "type": "JsonProperty", "json": { "a": [ 1, 2 ], "b": { "c": true, "d": null } } },
    "address": { "type": "Property", "value": { "street": "Unter den Linden 1", "city": "Berlin" } }
  }'
  req 201 POST /entities '{ "id": "urn:ngsi-ld:Vehicle:upg2", "type": "Vehicle", "scope": [ "/area/south", "/fleet/a" ], "speed": { "type": "Property", "value": 45, "observedAt": "'$T0'" }, "location": { "type": "GeoProperty", "value": { "type": "Point", "coordinates": [ 13.40, 52.52 ] } } }'
  req 201 POST /entities '{ "id": "urn:ngsi-ld:Vehicle:upg3", "type": "Vehicle", "speed": { "type": "Property", "value": 120 }, "location": { "type": "GeoProperty", "value": { "type": "Point", "coordinates": [ 2.35, 48.86 ] } } }'
  req 201 POST /entities '{ "id": "urn:ngsi-ld:Vehicle:upg4", "type": [ "Vehicle", "Truck" ], "speed": { "type": "Property", "value": 60 }, "location": { "type": "GeoProperty", "value": { "type": "Point", "coordinates": [ 13.36, 52.50 ] } } }'
  req 201 POST /entities '{ "id": "urn:ngsi-ld:Vehicle:upg5", "type": "Vehicle", "speed": { "type": "Property", "value": 10.5 }, "active": { "type": "Property", "value": false }, "seen": { "type": "Property", "value": { "@type": "DateTime", "@value": "2026-01-01T00:00:00.000Z" } } }'
  req 201 POST /entities '{ "id": "urn:ngsi-ld:Building:upg1", "type": "Building", "location": { "type": "GeoProperty", "value": { "type": "Polygon", "coordinates": [ [ [ 13.30, 52.48 ], [ 13.45, 52.48 ], [ 13.45, 52.55 ], [ 13.30, 52.55 ], [ 13.30, 52.48 ] ] ] } }, "floors": { "type": "Property", "value": 7 } }'
  req 201 POST /entities '{ "id": "urn:ngsi-ld:Sensor:upg1", "type": "Sensor", "temperature": { "type": "Property", "value": 21.5, "unitCode": "CEL", "observedAt": "'$T0'" }, "in": [ { "type": "Relationship", "object": "urn:ngsi-ld:Building:upg1" }, { "type": "Relationship", "object": "urn:ngsi-ld:Building:upg2", "datasetId": "urn:ngsi-ld:Dataset:b" } ] }'
  req 201 POST /entities '{ "id": "urn:ngsi-ld:Vehicle:upgT", "type": "Vehicle", "speed": { "type": "Property", "value": 33, "observedAt": "'$T0'" } }' -H "NGSILD-Tenant: $TENANT"

  # History: PATCHes with observedAt (the creates are history too)
  req 204 PATCH /entities/urn:ngsi-ld:Vehicle:upg1/attrs '{ "speed": { "type": "Property", "value": 85, "unitCode": "KMH", "observedAt": "'$T1'" } }'
  req 204 PATCH /entities/urn:ngsi-ld:Vehicle:upg1/attrs '{ "speed": { "type": "Property", "value": 90, "unitCode": "KMH", "observedAt": "'$T2'" } }'
  req 204 PATCH /entities/urn:ngsi-ld:Vehicle:upg1/attrs '{ "speed": { "type": "Property", "value": 83, "unitCode": "KMH", "observedAt": "'$T1'", "datasetId": "urn:ngsi-ld:Dataset:gps" } }'
  req 204 PATCH /entities/urn:ngsi-ld:Sensor:upg1/attrs '{ "temperature": { "type": "Property", "value": 22.0, "unitCode": "CEL", "observedAt": "'$T1'" } }'
  req 204 PATCH /entities/urn:ngsi-ld:Vehicle:upgT/attrs '{ "speed": { "type": "Property", "value": 34, "observedAt": "'$T1'" } }' -H "NGSILD-Tenant: $TENANT"

  # Subscriptions - after the entities, so nothing notifies before the receiver is up
  req 201 POST /subscriptions '{ "id": "urn:ngsi-ld:Subscription:upg1", "type": "Subscription", "subscriptionName": "fast vehicles", "description": "speed over 50", "entities": [ { "type": "Vehicle" } ], "watchedAttributes": [ "speed" ], "q": "speed>50", "notification": { "attributes": [ "speed", "location" ], "format": "normalized", "endpoint": { "uri": "http://localhost:'$RPORT'/notify", "accept": "application/json" } } }'
  req 201 POST /subscriptions '{ "id": "urn:ngsi-ld:Subscription:upg2", "type": "Subscription", "entities": [ { "type": "Vehicle" } ], "geoQ": { "geometry": "Point", "coordinates": [ 13.35, 52.51 ], "georel": "near;maxDistance==5000" }, "throttling": 5, "expiresAt": "2099-01-01T00:00:00.000Z", "notification": { "format": "keyValues", "endpoint": { "uri": "http://localhost:'$RPORT'/geo" } } }'
  req 201 POST /subscriptions '{ "id": "urn:ngsi-ld:Subscription:upg3", "type": "Subscription", "entities": [ { "idPattern": "urn:ngsi-ld:Sensor:.*", "type": "Sensor" } ], "notificationTrigger": [ "entityCreated", "attributeUpdated" ], "notification": { "endpoint": { "uri": "http://localhost:'$RPORT'/paused" } } }'
  req 204 PATCH /subscriptions/urn:ngsi-ld:Subscription:upg3 '{ "isActive": false }'
  req 201 POST /subscriptions '{ "id": "urn:ngsi-ld:Subscription:upgT", "type": "Subscription", "entities": [ { "type": "Vehicle" } ], "notification": { "endpoint": { "uri": "http://localhost:'$RPORT'/tenant" } } }' -H "NGSILD-Tenant: $TENANT"

  # Registrations - of a type the queries below do not ask for, so nothing is forwarded
  req 201 POST /csourceRegistrations '{ "id": "urn:ngsi-ld:ContextSourceRegistration:upg1", "type": "ContextSourceRegistration", "registrationName": "remote things", "information": [ { "entities": [ { "type": "RemoteThing" } ], "propertyNames": [ "p1" ], "relationshipNames": [ "r1" ] } ], "endpoint": "http://localhost:19999", "mode": "inclusive", "contextSourceInfo": [ { "key": "k1", "value": "v1" } ] }'
  req 201 POST /csourceRegistrations '{ "id": "urn:ngsi-ld:ContextSourceRegistration:upg2", "type": "ContextSourceRegistration", "information": [ { "entities": [ { "idPattern": "urn:ngsi-ld:RemoteGadget:.*", "type": "RemoteGadget" } ] } ], "endpoint": "http://localhost:19998", "mode": "auxiliary", "expiresAt": "2099-01-01T00:00:00.000Z" }'
}

#
# snapshot <dir> - every read, one file each: "<status>\n<body normalized>"
#
snapshot()
{
  local dir=$1
  mkdir -p "$dir"
  local n=0
  get() # get <name> <path> [curl args...]
  {
    local name=$1 path=$2; shift 2
    local status
    status=$(curl -s -o "$dir/$name.raw" -w '%{http_code}' "$BASE$path" "$@")
    { echo "$status"; python3 "$WORK/normalize.py" < "$dir/$name.raw"; } > "$dir/$name"
    rm -f "$dir/$name.raw"
  }
  for id in $ENTITY_IDS; do
    get "entity.$id"          "/entities/$id?options=sysAttrs"
    get "entity.$id.kv"       "/entities/$id?options=keyValues"
  done
  get "entity.tenant"         "/entities/urn:ngsi-ld:Vehicle:upgT?options=sysAttrs" -H "NGSILD-Tenant: $TENANT"
  get "query.type"            "/entities?type=Vehicle&options=sysAttrs&limit=100"
  get "query.types-multi"     "/entities?type=Truck&limit=100"
  get "query.q"               "/entities?type=Vehicle&q=speed>50&attrs=speed&limit=100"
  get "query.q-dataset"       "/entities?type=Vehicle&q=speed==82&limit=100"
  get "query.near"            "/entities?type=Vehicle&georel=near;maxDistance==5000&geometry=Point&coordinates=%5B13.35,52.51%5D&attrs=location&limit=100"
  get "query.within"          "/entities?type=Vehicle&georel=within&geometry=Polygon&coordinates=%5B%5B%5B13.30,52.48%5D,%5B13.45,52.48%5D,%5B13.45,52.55%5D,%5B13.30,52.55%5D,%5B13.30,52.48%5D%5D%5D&attrs=location&limit=100"
  get "query.intersects"      "/entities?type=Building&georel=intersects&geometry=Point&coordinates=%5B13.35,52.51%5D&limit=100"
  get "query.scopeQ"          "/entities?type=Vehicle&scopeQ=/area/north&limit=100"
  get "query.lang"            "/entities?type=Vehicle&attrs=name&lang=sv&limit=100"
  get "query.ids"             "/entities?type=Sensor,Building&id=urn:ngsi-ld:Sensor:upg1,urn:ngsi-ld:Building:upg1&limit=100"
  get "query.tenant"          "/entities?type=Vehicle&limit=100" -H "NGSILD-Tenant: $TENANT"
  get "types"                 "/types"
  get "attributes"            "/attributes"
  get "subscriptions"         "/subscriptions?limit=100"
  get "subscriptions.tenant"  "/subscriptions?limit=100" -H "NGSILD-Tenant: $TENANT"
  for s in upg1 upg2 upg3; do get "subscription.$s" "/subscriptions/urn:ngsi-ld:Subscription:$s"; done
  get "registrations"         "/csourceRegistrations?type=RemoteThing,RemoteGadget&limit=100"
  for r in upg1 upg2; do get "registration.$r" "/csourceRegistrations/urn:ngsi-ld:ContextSourceRegistration:$r"; done
  if [ "$STORE" == mongoc ]; then
    local t
    for t in "$DB" "$DB-$TENANT"; do
      mongosh --host $MONGO_HOST --port $MONGO_PORT --quiet --eval \
        "['entities','subscriptions','registrations'].forEach(c => printjson(db.getSiblingDB('$t')[c].find().sort({_id:1}).toArray()))" > "$dir/raw.$t"
    done
  fi
  if [ "$TROE" == 1 ]; then
    get "temporal.V1"         "/temporal/entities/urn:ngsi-ld:Vehicle:upg1"
    get "temporal.V1.sys"     "/temporal/entities/urn:ngsi-ld:Vehicle:upg1?options=sysAttrs"
    get "temporal.S1"         "/temporal/entities/urn:ngsi-ld:Sensor:upg1"
    get "temporal.after"      "/temporal/entities/urn:ngsi-ld:Vehicle:upg1?timerel=after&timeAt=$T0&attrs=speed"
    get "temporal.query"      "/temporal/entities?type=Vehicle&timerel=before&timeAt=2030-01-01T00:00:00.000Z&attrs=speed&limit=100"
    get "temporal.tenant"     "/temporal/entities/urn:ngsi-ld:Vehicle:upgT" -H "NGSILD-Tenant: $TENANT"
  fi
}

#
# KNOWN - a read that differs between two releases ON PURPOSE: "<from version>:<read>" -> why. Shown,
# not counted.
#
declare -A KNOWN
KNOWN["0.4.0:temporal.V1.sys"]="the entity's modifiedAt in a temporal representation: 0.4.0 rendered its creation time, 0.5.0 its last modification"
FROM_VERSION=${FROM##*:}

#
# compare <a> <b> <label> <failKind> - every file of snapshot a against b
#
compare()
{
  local a=$1 b=$2 label=$3 kind=$4
  local f diffs=0
  for f in $(ls "$a"); do
    if ! diff -q "$a/$f" "$b/$f" > /dev/null 2>&1; then
      if [ "$kind" == fail ] && [ -n "${KNOWN[$FROM_VERSION:$f]}" ]; then
        echo "   known $label: $f - ${KNOWN[$FROM_VERSION:$f]}"
        continue
      fi
      diffs=$((diffs + 1))
      if [ "$kind" == fail ]; then bad "$label: $f differs"; else echo "   DOWNGRADE  $label: $f differs"; DOWNGRADE_DIFFS=$((DOWNGRADE_DIFFS + 1)); fi
      diff -u "$a/$f" "$b/$f" | head -40 | sed 's/^/          /'
    fi
  done
  [ $diffs == 0 ] && ok "$label: all $(ls "$a" | wc -l) reads identical"
}

#
# The reads that must return something particular - on the 'after' snapshot
#
checkQueries()
{
  local dir=$1
  expectIds() # expectIds <file> <ids...>
  {
    local f=$1; shift
    local got want
    got=$(tail -n +2 "$dir/$f" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(" ".join(sorted(e["id"] for e in d)))' 2>/dev/null)
    want=$(printf '%s\n' "$@" | sort | tr '\n' ' ' | sed 's/ $//')
    if [ "$got" == "$want" ] && [ "$(head -1 "$dir/$f")" == 200 ]; then ok "$f: $want"; else bad "$f: got '$got', expected '$want'"; fi
  }
  expectIds query.type        urn:ngsi-ld:Vehicle:upg1 urn:ngsi-ld:Vehicle:upg2 urn:ngsi-ld:Vehicle:upg3 urn:ngsi-ld:Vehicle:upg4 urn:ngsi-ld:Vehicle:upg5
  expectIds query.types-multi urn:ngsi-ld:Vehicle:upg4
  expectIds query.q           urn:ngsi-ld:Vehicle:upg1 urn:ngsi-ld:Vehicle:upg3 urn:ngsi-ld:Vehicle:upg4
  expectIds query.near        urn:ngsi-ld:Vehicle:upg1 urn:ngsi-ld:Vehicle:upg2 urn:ngsi-ld:Vehicle:upg4
  expectIds query.within      urn:ngsi-ld:Vehicle:upg1 urn:ngsi-ld:Vehicle:upg2 urn:ngsi-ld:Vehicle:upg4
  expectIds query.intersects  urn:ngsi-ld:Building:upg1
  expectIds query.scopeQ      urn:ngsi-ld:Vehicle:upg1
  expectIds query.ids         urn:ngsi-ld:Building:upg1 urn:ngsi-ld:Sensor:upg1
  expectIds query.tenant      urn:ngsi-ld:Vehicle:upgT
}

#
# expectHistory <dir> <file> <attr> <value> <observedAt> - the history holds that instance
#
expectHistory()
{
  local dir=$1 f=$2 attr=$3 value=$4 at=$5
  if tail -n +2 "$dir/$f" | python3 -c '
import json, sys
d = json.load(sys.stdin); a = d.get(sys.argv[1], [])
a = a if isinstance(a, list) else [a]
sys.exit(0 if any(i.get("value") == json.loads(sys.argv[2]) and i.get("observedAt", "").startswith(sys.argv[3][:19]) for i in a) else 1)
' "$attr" "$value" "$at" 2>/dev/null; then ok "$f: $attr $value at $at"; else bad "$f: no $attr $value at $at in the history"; fi
}

#
# The receiver: every POST body appended to $WORK/notifications, one line each
#
receiverStart()
{
  python3 - "$RPORT" "$WORK/notifications" <<'EOF' > /dev/null 2>&1 &
import http.server, sys
port, out = int(sys.argv[1]), sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        with open(out, 'ab') as f: f.write(self.path.encode() + b' ' + body.replace(b'\n', b' ') + b'\n')
        self.send_response(200); self.send_header('Content-Length', '0'); self.end_headers()
    def log_message(self, *a): pass
http.server.HTTPServer(('127.0.0.1', port), H).serve_forever()
EOF
  RECEIVER_PID=$!
  sleep 0.5
}

receiverStop() { [ -n "$RECEIVER_PID" ] && kill $RECEIVER_PID 2>/dev/null; RECEIVER_PID=""; }

cleanup()
{
  receiverStop
  newStop
  oldStop
  if [ "$KEEP" == 0 ]; then
    dropStores
    rm -rf "$WORK"
  else
    echo "kept: $WORK, mongo db $DB*, postgres db $PGDB*"
  fi
}
trap cleanup EXIT

dropStores()
{
  mongosh --host $MONGO_HOST --port $MONGO_PORT --quiet --eval \
    "db.adminCommand({listDatabases:1}).databases.map(d=>d.name).filter(n=>n=='$DB'||n.startsWith('$DB-')||n.startsWith('${DB}_')).forEach(n=>db.getSiblingDB(n).dropDatabase())" > /dev/null 2>&1
  local d
  for d in $(psql -h $PG_HOST -p $PG_PORT -U $PG_USER -tAc "SELECT datname FROM pg_database WHERE datname = '$PGDB' OR datname LIKE '${PGDB}\\_%'" 2>/dev/null); do
    psql -h $PG_HOST -p $PG_PORT -U $PG_USER -c "DROP DATABASE IF EXISTS \"$d\"" > /dev/null 2>&1
  done
  rm -rf "$WORK/cordb"
}

cat > "$WORK/normalize.py" <<'EOF'
# The body as sorted JSON - an array of objects with ids sorted by id - or verbatim if not JSON
import json, sys
raw = sys.stdin.read()
try:
    d = json.loads(raw)
except Exception:
    print(raw); sys.exit(0)
if isinstance(d, list) and all(isinstance(e, dict) and 'id' in e for e in d):
    d = sorted(d, key=lambda e: e['id'])
#
# The instances of a temporal attribute in a stable order: instances with the same observedAt come in
# no promised order (and did not in the same one across releases)
#
def stable(x):
    if isinstance(x, dict):
        return {k: stable(v) for k, v in x.items()}
    if isinstance(x, list):
        x = [stable(v) for v in x]
        if x and all(isinstance(v, dict) and 'instanceId' in v for v in x):
            x = sorted(x, key=lambda v: (v.get('observedAt', ''), v['instanceId']))
        return x
    return x
d = stable(d)
#
# A subscription's notification counters (timesSent, lastNotification, ...) live in memory - a restart
# resets them, in every release - so they are not part of what an upgrade has to keep
#
def counters(x):
    if isinstance(x, list):
        return [counters(v) for v in x]
    if isinstance(x, dict):
        if x.get('type') == 'Subscription' and isinstance(x.get('notification'), dict):
            for k in ('timesSent', 'timesFailed', 'lastNotification', 'lastSuccess', 'lastFailure', 'status'):
                x['notification'].pop(k, None)
        return x
    return x
d = counters(d)
print(json.dumps(d, indent=2, sort_keys=True))
EOF

#
# One store, all five steps
#
runStore()
{
  local store=$1
  STORE=$store
  local -a oldArgs newArgs
  case $store in
    mongoc)
      TROE=1
      oldArgs=(--database mongoc --dbName $DB --dbHost $MONGO_HOST --dbPort $MONGO_PORT --troe timescale --troeName $PGDB --troeUser $PG_USER --troeHost $PG_HOST --troePort $PG_PORT)
      newArgs=("${oldArgs[@]}")
      psql -h $PG_HOST -p $PG_PORT -U $PG_USER -c "CREATE DATABASE $PGDB" > /dev/null || { bad "cannot create the postgres database $PGDB"; return; }
      ;;
    corDB)
      TROE=1
      mkdir -p "$WORK/cordb"
      oldArgs=(--database corDB --dbDir "$WORK/cordb" --troe corDB)
      newArgs=("${oldArgs[@]}")
      ;;
  esac

  say "$store: 1+2. $FROM writes and reads"
  oldStart "$store.old" "${oldArgs[@]}" || { bad "$FROM did not start"; return; }
  write
  sleep 1                                                     # deferred TRoE writes land
  snapshot "$WORK/$store.before"
  if [ "$store" == mongoc ]; then
    mongosh --host $MONGO_HOST --port $MONGO_PORT --quiet --eval "db.getSiblingDB('$DB').entities.getIndexes().map(i=>i.name).join(' ')" > "$WORK/indexes.before"
    echo "   indexes ($FROM): $(cat "$WORK/indexes.before")"
  fi
  oldStop

  say "$store: 3. $TO reads"
  newStart "$store.new" "${newArgs[@]}" || { bad "$TO did not start on the data of $FROM"; return; }
  echo "   $($TO --version | head -1)"
  snapshot "$WORK/$store.after"
  compare "$WORK/$store.before" "$WORK/$store.after" "$store: $FROM -> $TO" fail

  say "$store: 4. $TO - queries, indexes, log, notification, history"
  checkQueries "$WORK/$store.after"
  if [ "$store" == mongoc ]; then
    local idx t
    for t in "$DB" "$DB-$TENANT"; do
      idx=$(mongosh --host $MONGO_HOST --port $MONGO_PORT --quiet --eval "db.getSiblingDB('$t').entities.getIndexes().map(i=>JSON.stringify(i.key)).join(' ')")
      echo "   indexes $t ($TO): $idx"
      [ -z "$idx" ] && continue
      if echo "$idx" | grep -q '{"type":1,"createdAt":1,"_id":1}'; then ok "$t: {type,createdAt,_id} created"; else bad "$t: no {type,createdAt,_id} index"; fi
      if echo "$idx" | grep -q '{"type":1} \|{"type":1}$'; then bad "$t: the old type_1 index is still there"; else ok "$t: type_1 gone"; fi
    done
  fi
  receiverStart
  req 204 PATCH /entities/urn:ngsi-ld:Vehicle:upg1/attrs '{ "speed": { "type": "Property", "value": 99, "unitCode": "KMH", "observedAt": "'$T3'" } }'
  local i
  for i in $(seq 1 100); do grep -q 'urn:ngsi-ld:Vehicle:upg1' "$WORK/notifications" 2>/dev/null && break; sleep 0.1; done
  if grep -q '^/notify .*urn:ngsi-ld:Subscription:upg1' "$WORK/notifications" 2>/dev/null; then ok "subscription upg1 notified on a PATCH"; else bad "subscription upg1 did not notify on a PATCH: $(cat "$WORK/notifications" 2>/dev/null)"; fi
  if grep -q '^/paused ' "$WORK/notifications" 2>/dev/null; then bad "the paused subscription upg3 notified"; fi
  receiverStop
  sleep 1
  local errs
  errs=$(grep -E '^(E|X)[:(]' "$WORK/$store.new.log" | grep -vE ': 4[0-9][0-9] ')        # not the requests refused
  if [ -z "$errs" ]; then ok "no E/X lines in the log of $TO"; else bad "the log of $TO has errors:"; echo "$errs" | head -20 | sed 's/^/          /'; fi
  grep -E '^W[:(]' "$WORK/$store.new.log" | head -10 | sed 's/^/   warn  /'
  snapshot "$WORK/$store.final"
  #
  # The datasetId instance --from modified, untouched by --to's PATCH of the default instance: its value
  # and its createdAt / modifiedAt as they were
  #
  local gps
  gps=$(for f in after final; do tail -n +2 "$WORK/$store.$f/entity.urn:ngsi-ld:Vehicle:upg1" | python3 -c '
import json, sys
s = json.load(sys.stdin)["speed"]
print([i for i in (s if isinstance(s, list) else [s]) if i.get("datasetId") == "urn:ngsi-ld:Dataset:gps"])'; done | sort -u | wc -l)
  if [ "$gps" == 1 ]; then ok "the gps instance of V1 unchanged by a PATCH of the default instance (sysAttrs too)"; else bad "the gps instance of V1 changed when $TO PATCHed the default instance:"; for f in after final; do tail -n +2 "$WORK/$store.$f/entity.urn:ngsi-ld:Vehicle:upg1" | python3 -c 'import json,sys; s=json.load(sys.stdin)["speed"]; print("          '$f':", [i for i in s if i.get("datasetId")])'; done; fi
  if [ "$TROE" == 1 ]; then
    expectHistory "$WORK/$store.final" temporal.V1 speed 99 $T3                          # written by --to
    expectHistory "$WORK/$store.final" temporal.S1 temperature 22.0 $T1                  # written by --from
    expectHistory "$WORK/$store.final" temporal.tenant speed 34 $T1
  fi
  newStop

  say "$store: 5. $FROM again - the downgrade"
  oldStart "$store.down" "${oldArgs[@]}" || { echo "   DOWNGRADE  $FROM does not start on the data $TO has touched"; DOWNGRADE_DIFFS=$((DOWNGRADE_DIFFS + 1)); return; }
  snapshot "$WORK/$store.down"
  compare "$WORK/$store.final" "$WORK/$store.down" "$store: $TO -> $FROM" downgrade
  oldStop
}

dropStores
if [ "$STORES" == all ] || [ "$STORES" == mongoc ]; then
  runStore mongoc
  [ "$KEEP" == 0 ] && dropStores
fi
if [ "$STORES" == all ] || [ "$STORES" == corDB ]; then
  if docker run --rm --entrypoint coraine "$FROM" --database corDB -u 2>&1 | grep -q -- '--dbDir'; then
    runStore corDB
  else
    say "corDB: not applicable - $FROM has no corDB persistence (no --dbDir): nothing it writes outlives it"
  fi
fi

echo
echo "upgradeCheck $FROM -> $TO: $FAILS failed check(s); downgrade: $DOWNGRADE_DIFFS difference(s)"
exit $FAILS
