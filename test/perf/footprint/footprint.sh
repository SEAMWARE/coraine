#!/usr/bin/env bash
#
# footprint.sh <outDir> - memory, start time and store size: the Data Space Connector's provider side
# (docker-compose.yml: Scorpio on PostGIS, APISIX + OPA + odrl-pap, VCVerifier, TIL, CCS, PostgreSQL) against
# coraine with --auth fdsc and the PAP's /policy API (--policyApiPort), on its durable store (corDB --dbDir)
#
# For each: started, time until it answers (the broker: GET /ngsi-ld/v1/entities?type=T is 200), 60 s to
# settle, the memory of every process; then the same 10 000 entities (100 batch creates of 100, five
# attributes each, through the broker's own port), 30 s, the memory again, and the store's size on disk.
#
# Memory: each process in a cgroup of its own - a container's, and coraine's a transient systemd scope -
# memory.current less inactive_file (what docker stats shows): what the process holds, without the page
# cache it could give back.
#
# Results: <outDir>/footprint.json and a table on stdout. Needs docker (compose), python3, systemd-run --user,
# coraine on PATH. Refuses while Chrome runs.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: footprint.sh <outDir>}
TOOLS=$HERE/../../funcTests/tools
ENTITIES=${FP_ENTITIES:-10000}
COMPOSE="docker compose -p footprint -f $HERE/docker-compose.yml"
export FP_WORK=$OUT/work
CORAINE_PORT=1027
CORAINE_POLICY_PORT=1029
SCOPE=footprint-coraine

die() { echo "footprint.sh: $*" >&2; exit 2; }
say() { echo "footprint.sh: $*" >&2; }
cleanup() {
  $COMPOSE down -v > /dev/null 2>&1
  systemctl --user stop "$SCOPE.scope" > /dev/null 2>&1
}
trap cleanup EXIT

pgrep -x chrome > /dev/null && die "Chrome is running - no measurement while it does"
for port in $CORAINE_PORT $CORAINE_POLICY_PORT 8080 9080 9090; do
  ss -Hltn "sport = :$port" | grep -q . && die "port $port is in use - $(ss -Hltnp "sport = :$port" | head -1)"
done

rm -rf "$OUT" && mkdir -p "$FP_WORK/plugins" "$OUT"
#
# odrl-authorization 2.4.3's opa plugin, as gatewayCompare.sh fetches it
#
for f in apixis-opa.lua:opa.lua apixis-helper.lua:helper.lua; do
  curl -sf "https://raw.githubusercontent.com/FIWARE/odrl-authorization/odrl-authorization-2.4.3/charts/odrl-authorization/files/${f%%:*}" \
       -o "$FP_WORK/plugins/${f##*:}" || die "cannot fetch ${f%%:*} (odrl-authorization-2.4.3)"
done
chmod -R a+rX "$FP_WORK"

#
# mem <cgroup dir> - bytes, as docker stats counts them
#
mem() { python3 - "$1" <<'EOF'
import sys
d = sys.argv[1]
cur = int(open(d + "/memory.current").read())
inactive = 0
for line in open(d + "/memory.stat"):
    k, v = line.split()
    if k == "inactive_file":
        inactive = int(v)
print(cur - inactive)
EOF
}
containerCg() { echo "/sys/fs/cgroup/system.slice/docker-$(docker inspect -f '{{.Id}}' "$1").scope"; }

#
# entities <base url> - ENTITIES entities, 100 a batch create; the seconds it took
#
entities() { python3 - "$1" "$ENTITIES" <<'EOF'
import json, os, sys, time, urllib.request
url, n = sys.argv[1], int(sys.argv[2])
t0 = time.time()
for b in range(0, n, 100):
    batch = [{"id": "urn:ngsi-ld:Vehicle:fp-%d" % i, "type": "Vehicle",
              "brand": {"type": "Property", "value": "Mercedes"},
              "speed": {"type": "Property", "value": i % 120, "observedAt": "2026-10-14T10:00:00Z"},
              "location": {"type": "GeoProperty", "value": {"type": "Point", "coordinates": [13.4 + i * 1e-5, 52.5]}},
              "isParked": {"type": "Relationship", "object": "urn:ngsi-ld:OffStreetParking:1"},
              "description": {"type": "Property", "value": "a five-attribute vehicle used for the footprint measure"}}
             for i in range(b, min(b + 100, n))]
    h = {"Content-Type": "application/json"}
    if os.environ.get("FP_AUTH"):
        h["Authorization"] = os.environ["FP_AUTH"]
    r = urllib.request.Request(url + "/ngsi-ld/v1/entityOperations/create", data=json.dumps(batch).encode(),
                               headers=h, method="POST")
    with urllib.request.urlopen(r, timeout=120) as resp:
        if resp.status not in (200, 201, 204):
            sys.exit("batch %d: %d" % (b, resp.status))
print("%.1f" % (time.time() - t0))
EOF
}
ready() { [ "$(curl -s -o /dev/null -w '%{http_code}' ${FP_AUTH:+-H "Authorization: $FP_AUTH"} "$1/ngsi-ld/v1/entities?type=Vehicle")" = 200 ]; }
awaitReady() { local t0=$(date +%s.%N); for _ in $(seq 1 600); do ready "$1" && { python3 -c "print('%.1f' % ($(date +%s.%N) - $t0))"; return 0; }; sleep 0.5; done; return 1; }

declare -A IDLE LOADED
RESULT="{}"

# =====================================================================================================================
#
# The Data Space Connector's provider side
#
say "the connector's provider side: compose up"
$COMPOSE pull -q > "$OUT/pull.log" 2>&1 || { cat "$OUT/pull.log"; die "pull"; }
t0=$(date +%s.%N)
$COMPOSE up -d > "$OUT/compose.log" 2>&1 || { cat "$OUT/compose.log"; die "compose up"; }
fdscReady=$(awaitReady http://localhost:9090) || die "Scorpio did not answer in 300 s - $COMPOSE logs scorpio"
fdscReady=$(python3 -c "print('%.1f' % ($(date +%s.%N) - $t0))")
say "Scorpio answers after ${fdscReady} s (from compose up); 60 s to settle"
sleep 60
SERVICES=$($COMPOSE config --services)
for s in $SERVICES; do
  c=$($COMPOSE ps -a -q "$s")
  state=$(docker inspect -f '{{.State.Status}}' "$c" 2>/dev/null)
  [ "$state" = running ] || { say "$s is $state - $(docker logs --tail 3 "$c" 2>&1 | tr '\n' ' ' | cut -c1-200)"; IDLE[$s]=null; continue; }
  IDLE[$s]=$(mem "$(containerCg "$c")")
done
say "creating $ENTITIES entities in Scorpio"
fdscLoad=$(entities http://localhost:9090) || die "the entities into Scorpio"
sleep 30
for s in $SERVICES; do
  c=$($COMPOSE ps -q "$s"); [ "${IDLE[$s]}" = null ] && { LOADED[$s]=null; continue; }
  LOADED[$s]=$(mem "$(containerCg "$c")")
done
fdscStore=$(docker exec "$($COMPOSE ps -q scorpio-db)" psql -U ngb -d ngb -tAc "select pg_database_size('ngb')")
$COMPOSE logs > "$OUT/fdsc.log" 2>&1
$COMPOSE down -v > /dev/null 2>&1

FDSC_JSON=$(for s in $SERVICES; do printf '"%s": {"idle": %s, "loaded": %s}\n' "$s" "${IDLE[$s]}" "${LOADED[$s]}"; done | paste -sd, -)

# =====================================================================================================================
#
# coraine: the broker, --auth fdsc and the PAP's /policy API in one process, on its durable store - the same 20
# policies as gatewayCompare.sh (perfPolicies.py 20), the creates with a token
#
python3 "$TOOLS/ftJwt.py" key --out "$FP_WORK" --name k1 --alg RS256 > /dev/null || die "no key"
python3 "$TOOLS/ftJwt.py" jwks --out "$FP_WORK/jwks.json" "$FP_WORK/k1.jwk"
python3 "$HERE/../perfPolicies.py" 20 > "$FP_WORK/policies.json"
TOKEN=$(python3 "$TOOLS/ftJwt.py" sign --key "$FP_WORK/k1.pem" --iss http://localhost:7740 --exp 86400 \
          --vc '{"type": ["VerifiableCredential", "UserCredential"], "issuer": "did:web:consumer", "credentialSubject": {}}')
export FP_AUTH="Bearer $TOKEN"
mkdir -p "$FP_WORK/db"

say "coraine: --auth fdsc, --policyApiPort, corDB --dbDir"
t0=$(date +%s.%N)
systemd-run --user --scope --unit "$SCOPE" -q \
  coraine -fg --port $CORAINE_PORT --database corDB --dbDir "$FP_WORK/db" \
          --auth fdsc --fdscJwks "$FP_WORK/jwks.json" --fdscPolicies "$FP_WORK/policies.json" \
          --fdscIssuer http://localhost:7740 --fdscOrganization did:web:provider \
          --policyApiPort $CORAINE_POLICY_PORT > "$OUT/coraine.log" 2>&1 &
corReady=$(awaitReady http://localhost:$CORAINE_PORT) || die "coraine did not answer - $OUT/coraine.log"
corReady=$(python3 -c "print('%.2f' % ($(date +%s.%N) - $t0))")
CG=$(find /sys/fs/cgroup/user.slice -type d -name "$SCOPE.scope" | head -1)
[ -n "$CG" ] || die "no cgroup for coraine"
say "coraine answers after ${corReady} s; 60 s to settle"
sleep 60
corIdle=$(mem "$CG")
say "creating $ENTITIES entities in coraine"
corLoad=$(entities http://localhost:$CORAINE_PORT) || die "the entities into coraine"
sleep 30
corLoaded=$(mem "$CG")
corStore=$(du -sb "$FP_WORK/db" | cut -f1)
systemctl --user stop "$SCOPE.scope"

# =====================================================================================================================
#
# The result
#
cat > "$OUT/footprint.json" <<JSON
{ "entities": $ENTITIES,
  "fdsc":    { "readySeconds": $fdscReady, "loadSeconds": $fdscLoad, "storeBytes": $fdscStore, "containers": { $FDSC_JSON } },
  "coraine": { "readySeconds": $corReady, "loadSeconds": $corLoad, "storeBytes": $corStore, "idle": $corIdle, "loaded": $corLoaded } }
JSON
python3 - "$OUT/footprint.json" <<'EOF2'
import json, sys
d = json.load(open(sys.argv[1]))
M = lambda b: "-" if b is None else "%.0f" % (b / 1048576)
print("%-14s %10s %10s" % ("process", "idle MiB", "loaded MiB"))
tot = [0, 0]
for name, c in d["fdsc"]["containers"].items():
    print("%-14s %10s %10s" % (name, M(c["idle"]), M(c["loaded"])))
    tot[0] += c["idle"] or 0; tot[1] += c["loaded"] or 0
print("%-14s %10s %10s" % ("FDSC total", M(tot[0]), M(tot[1])))
print("%-14s %10s %10s" % ("coraine", M(d["coraine"]["idle"]), M(d["coraine"]["loaded"])))
for k in ("fdsc", "coraine"):
    print("%s: ready after %s s, %d entities in %s s, store %.1f MiB" % (k, d[k]["readySeconds"], d["entities"], d[k]["loadSeconds"], d[k]["storeBytes"] / 1048576))
EOF2
