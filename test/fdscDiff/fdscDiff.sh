#!/bin/bash
#
# FILE            fdscDiff.sh
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# fdsc.so against the Data Space Connector itself (doc/authorization.md § 4.3): the connector's provider-side
# chain as FDSC 10.87.0 deploys it - APISIX 3.17.0 with openid-connect and odrl-authorization 2.4.3's opa
# plugin, OPA 1.11.0, odrl-pap 1.7.3 - and coraine with --auth fdsc, given the same verifier and the same
# PAP; every case of cases.json sent to both, the answers compared by class (fdscDiff.py).
#
# The verifier is a stand-in (test/funcTests/tools/ftVerifier.py: the discovery document and JWK Set,
# as VCVerifier 6.23.1 serves them); the tokens are made by ftJwt.py as VCVerifier makes them.
#
# Needs: docker (compose), python3, openssl, curl, and coraine with its plugins installed - or, with
# FDSC_DIFF_CORAINE_IMAGE=<image>, a coraine image: both brokers run from it (host network). Uses the
# ports 1027, 1028 (brokers), 5440 (PostgreSQL), 7740 (verifier), 8080 + 9001 (PAP), 8181 (OPA), 9080
# (APISIX).
#
#   test/fdscDiff/fdscDiff.sh [--keep]      --keep: leave everything running afterwards
#
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$HERE/../funcTests/tools
export FDSC_DIFF_WORK=${FDSC_DIFF_WORK:-/tmp/fdscDiff}
ODRL_AUTHZ_TAG=odrl-authorization-2.4.3
KEEP=0
[ "${1:-}" == "--keep" ] && KEEP=1

VERIFIER=http://localhost:7740
PAP=http://localhost:8080
PIDS=()

IMAGE=${FDSC_DIFF_CORAINE_IMAGE:-}

cleanup() {
  [ $KEEP == 1 ] && return
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  [ -n "$IMAGE" ] && docker rm -f fdscdiff-plain fdscdiff-coraine > /dev/null 2>&1
  docker compose -f "$HERE/docker-compose.yml" down -v > /dev/null 2>&1
}
trap cleanup EXIT

die() { echo "fdscDiff: $*" >&2; exit 2; }

await() {   # await <seconds> <command...> - until the command succeeds
  local secs=$1; shift
  for i in $(seq 1 $((secs * 5))); do "$@" > /dev/null 2>&1 && return 0; sleep 0.2; done
  return 1
}

#
# Keys, the verifier, the connector's opa plugin
#
rm -rf "$FDSC_DIFF_WORK" && mkdir -p "$FDSC_DIFF_WORK/plugins"
python3 "$TOOLS/ftJwt.py" key --out "$FDSC_DIFF_WORK" --name k1 --alg RS256 || die "no key"
python3 "$TOOLS/ftJwt.py" key --out "$FDSC_DIFF_WORK" --name k2 --alg ES256 || die "no key"
python3 "$TOOLS/ftJwt.py" jwks --out "$FDSC_DIFF_WORK/jwks.json" "$FDSC_DIFF_WORK/k1.jwk" "$FDSC_DIFF_WORK/k2.jwk"
echo '[]' > "$FDSC_DIFF_WORK/papListing.json"

for f in apixis-opa.lua:opa.lua apixis-helper.lua:helper.lua; do
  curl -sf "https://raw.githubusercontent.com/FIWARE/odrl-authorization/$ODRL_AUTHZ_TAG/charts/odrl-authorization/files/${f%%:*}" \
       -o "$FDSC_DIFF_WORK/plugins/${f##*:}" || die "cannot fetch ${f%%:*} ($ODRL_AUTHZ_TAG)"
done

python3 "$TOOLS/ftVerifier.py" --port 7740 --dir "$FDSC_DIFF_WORK" > "$FDSC_DIFF_WORK/verifier.log" 2>&1 &
PIDS+=($!)

#
# The connector's chain
#
#
# PostgreSQL first, until it answers a query over TCP - its image's entrypoint runs a server of its own
# during the initialisation
#
COMPOSE="docker compose -f $HERE/docker-compose.yml"
$COMPOSE up -d postgres > "$FDSC_DIFF_WORK/compose.log" 2>&1 || { cat "$FDSC_DIFF_WORK/compose.log"; die "compose up postgres"; }
await 60 $COMPOSE exec -T postgres psql -h localhost -p 5440 -U postgres -d pap -c "select 1" || die "PostgreSQL did not come up"
$COMPOSE up -d >> "$FDSC_DIFF_WORK/compose.log" 2>&1 || { cat "$FDSC_DIFF_WORK/compose.log"; die "compose up"; }
#
# The PAP connects to PostgreSQL once, at start, and exits when it cannot: started again until it is up
#
papUp() {
  curl -sf "$PAP/policy" > /dev/null && return 0
  [ "$($COMPOSE ps -a --format '{{.State}}' pap)" == "exited" ] && $COMPOSE start pap > /dev/null 2>&1
  return 1
}
await 120 papUp || die "the PAP did not come up - docker compose logs pap"

n=0
for i in $(python3 -c "import json; print(len(json.load(open('$HERE/policies.json'))))" | xargs seq 0 | head -n -1); do
  python3 -c "import json,sys; json.dump(json.load(open('$HERE/policies.json'))[$i], sys.stdout)" | \
    curl -sf -X POST "$PAP/policy" -H "Content-Type: application/json" --data-binary @- -o /dev/null || die "the PAP refused policy $i"
  n=$((n + 1))
done
echo "fdscDiff: $n policies in the PAP"

#
# The brokers: the plain one behind APISIX, and coraine with --auth fdsc
#
broker() {   # broker <name> <options...> - on the host, or from the image
  local name=$1; shift
  if [ -n "$IMAGE" ]; then
    docker rm -f "fdscdiff-$name" > /dev/null 2>&1
    docker run -d --name "fdscdiff-$name" --network host "$IMAGE" "$@" > /dev/null || die "cannot start the $name broker"
  else
    coraine -fg "$@" > "$FDSC_DIFF_WORK/$name.log" 2>&1 &
    PIDS+=($!)
  fi
}

broker plain   --port 1027 --database corDB
broker coraine --port 1028 --database corDB --auth fdsc --fdscDiscovery "$VERIFIER/services/data-service/.well-known/openid-configuration" \
               --fdscPap "$PAP" --fdscOrganization did:web:provider --fdscRefresh 2

TOKEN=$(python3 "$TOOLS/ftJwt.py" sign --key "$FDSC_DIFF_WORK/k1.pem" --iss "$VERIFIER")
ready() { s=$(curl -s -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $TOKEN" "$1/ngsi-ld/v1/entities/urn:ngsi-ld:EnergyReport:1"); [ "$s" == "404" ]; }
await 60 ready http://localhost:9080 || die "the connector's chain does not let the first case through - docker compose logs apisix opa"
await 30 ready http://localhost:1028 || { [ -n "$IMAGE" ] && docker logs fdscdiff-coraine 2>&1 | tail -20; die "coraine does not let the first case through - $FDSC_DIFF_WORK/coraine.log"; }
sleep 3   # OPA and coraine poll the PAP: both have every policy

python3 "$HERE/fdscDiff.py" --apisix http://localhost:9080 --coraine http://localhost:1028 --cases "$HERE/cases.json" \
        --keys "$FDSC_DIFF_WORK" --iss "$VERIFIER" --known "$HERE/known.json"
