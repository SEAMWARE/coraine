#!/usr/bin/env bash
#
# gatewayCompare.sh - the security of the broker against the security of a gateway in front of it, the same
# job: perfRun.sh on the same broker build, the same token, the same policies, in each of
#
#   none     the broker on its own, no token (security off)
#   fdsc     the broker with --auth fdsc: the token checked and the policies decided in the broker
#   apisix   APISIX in front of the broker, a route with no plugin: what the gateway alone costs
#   opa      the Data Space Connector's chain in front of the broker (no --auth): APISIX with openid-connect
#            (the token) and the opa plugin, OPA deciding on the ODRL-PAP's policies
#   plugin   APISIX in front of the broker (no --auth) with ngsild-pep: fdsc.so's own code deciding in the
#            gateway (SEAMWARE corApisixNgsild - GW_NGSILD, default ../../../corApisixNgsild beside coraine: its
#            dist/debian-12/libngsildpep.so and lua/), one FFI call per request
#
# Usage:  gatewayCompare.sh <outDir> [config ...]     (default: none fdsc apisix opa plugin none)
#
# Placement (PERF_BROKER_CORES=1, PERF_RESERVED_CORES=2 in every configuration, so that the load is the same):
# the broker on the first physical core, APISIX - one nginx worker - on the second, OPA - GOMAXPROCS=1 - on
# the third, the load generator on the rest. The gateway's chain is test/perf/gateway (docker, host network):
# PostgreSQL 5440, the PAP 8080 + 9001, OPA 8181, APISIX 9080; the verifier stand-in on 7740; the broker 1027.
#
# The policies: perfPolicies.py <GW_POLICIES> (default 20): the same set in the PAP (for OPA) and in a file
# (for fdsc.so). The token: RS256, a UserCredential, its iss the verifier - checked by openid-connect against
# the verifier's JWK Set, and by fdsc.so against the same keys.
#
# Everything else is perfRun.sh's (PERF_DB, PERF_DURATION, PERF_REPEATS, ...). Needs docker (compose), python3,
# curl, wrk; no Chrome running (perf).
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -uo pipefail

OUT=${1:?usage: gatewayCompare.sh <outDir> [config ...]}
shift
CONFIGS=( "${@:-none fdsc apisix opa plugin none}" )
read -r -a CONFIGS <<< "${CONFIGS[*]}"

HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$HERE/../funcTests/tools
GW=$HERE/gateway
N=${GW_POLICIES:-20}
DB=${PERF_DB:-corDB}
BROKER_PORT=1027
VERIFIER=http://localhost:7740
PAP=http://localhost:8080
ODRL_AUTHZ_TAG=odrl-authorization-2.4.3
export GW_WORK=$OUT/work
export PERF_BROKER_CORES=1 PERF_RESERVED_CORES=2
export GW_ROUTES=$GW/routes-plain.yaml
NGSILD=${GW_NGSILD:-$HERE/../../../corApisixNgsild}
COMPOSE="docker compose -p gwcompare -f $GW/docker-compose.yml"
PIDS=()

die() { echo "gatewayCompare.sh: $*" >&2; exit 2; }
await() { local s=$1; shift; for _ in $(seq 1 $((s * 5))); do "$@" > /dev/null 2>&1 && return 0; sleep 0.2; done; return 1; }
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  [ "${GW_KEEP:-0}" = 1 ] || $COMPOSE down -v > /dev/null 2>&1
}
trap cleanup EXIT

#
# Every port free before anything starts: a listener left from elsewhere (another APISIX on the host network) takes
# a share of the load, and the numbers are a mix that looks plausible
#
for port in $BROKER_PORT 5440 7740 8080 8181 9001 9080; do
  ss -Hltn "sport = :$port" | grep -q . && die "port $port is in use - $(ss -Hltnp "sport = :$port" | head -1)"
done

mkdir -p "$OUT" "$GW_WORK/plugins"

#
# The physical cores, as perfRun.sh reads them: the second for APISIX, the third for OPA
#
mapfile -t physCpus < <(
  for d in /sys/devices/system/cpu/cpu[0-9]*; do
    id=${d##*/cpu}; sib=$(cut -d, -f1 < "$d/topology/thread_siblings_list" 2>/dev/null || echo "$id")
    [ "$sib" = "$id" ] && echo "$id"
  done | sort -n)
export GW_APISIX_CPU=${physCpus[1]} GW_OPA_CPU=${physCpus[2]}

#
# Keys, token, policies, the verifier, the connector's opa plugin
#
python3 "$TOOLS/ftJwt.py" key  --out "$GW_WORK" --name k1 --alg RS256 > /dev/null || die "no key"
python3 "$TOOLS/ftJwt.py" jwks --out "$GW_WORK/jwks.json" "$GW_WORK/k1.jwk"
echo '[]' > "$GW_WORK/papListing.json"
TOKEN=$(python3 "$TOOLS/ftJwt.py" sign --key "$GW_WORK/k1.pem" --iss "$VERIFIER" --exp 86400 \
          --vc '{"type": ["VerifiableCredential", "UserCredential"], "issuer": "did:web:consumer", "credentialSubject": {}}')
python3 "$HERE/perfPolicies.py" "$N" > "$OUT/policies-$N.json"

for f in apixis-opa.lua:opa.lua apixis-helper.lua:helper.lua; do
  curl -sf "https://raw.githubusercontent.com/FIWARE/odrl-authorization/$ODRL_AUTHZ_TAG/charts/odrl-authorization/files/${f%%:*}" \
       -o "$GW_WORK/plugins/${f##*:}" || die "cannot fetch ${f%%:*} ($ODRL_AUTHZ_TAG)"
done

#
# ngsild-pep, for 'plugin': its library, its Lua, its configuration - the same keys and policies fdsc.so reads
#
if [[ " ${CONFIGS[*]} " == *" plugin "* ]]; then
  [ -f "$NGSILD/dist/debian-12/libngsildpep.so" ] || die "no $NGSILD/dist/debian-12/libngsildpep.so ($NGSILD/build-in-docker.sh)"
  cp "$NGSILD/dist/debian-12/libngsildpep.so" "$GW_WORK/"
  cp "$NGSILD/lua/apisix/plugins/ngsild-pep.lua" "$GW_WORK/plugins/"
  cp "$OUT/policies-$N.json" "$GW_WORK/policies.json"
  gwArgs=$(printf '"%s", ' --fdscJwks /opts/ngsild/jwks.json --fdscPolicies /opts/ngsild/policies.json --fdscIssuer "$VERIFIER" --fdscOrganization did:web:provider)
  sed -e "s|@WORKERS@|1|" -e "s|@ARGS@|${gwArgs%, }|" "$NGSILD/test/config.yaml.in" > "$GW_WORK/config-ngsild.yaml"
  chmod -R a+rX "$GW_WORK"                           # APISIX runs as its own user in the container
fi

python3 "$TOOLS/ftVerifier.py" --port 7740 --dir "$GW_WORK" > "$GW_WORK/verifier.log" 2>&1 &
PIDS+=($!)

#
# The chain: PostgreSQL, the PAP (the policies), OPA; APISIX per configuration
#
$COMPOSE up -d postgres > "$GW_WORK/compose.log" 2>&1 || die "compose up postgres ($GW_WORK/compose.log)"
await 60 $COMPOSE exec -T postgres psql -h localhost -p 5440 -U postgres -d pap -c "select 1" || die "PostgreSQL did not come up"
$COMPOSE up -d pap opa >> "$GW_WORK/compose.log" 2>&1 || die "compose up pap opa"
papUp() {
  curl -sf "$PAP/policy" > /dev/null && return 0
  [ "$($COMPOSE ps -a --format '{{.State}}' pap)" == "exited" ] && $COMPOSE start pap > /dev/null 2>&1
  return 1
}
await 120 papUp || die "the PAP did not come up - $COMPOSE logs pap"
python3 - "$OUT/policies-$N.json" "$PAP" <<'EOF' || die "the PAP refused a policy"
import json, sys, urllib.request
for p in json.load(open(sys.argv[1])):
    r = urllib.request.Request(sys.argv[2] + "/policy", data=json.dumps(p).encode(), headers={"Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(r).read()
EOF
echo "gatewayCompare.sh: $N policies in the PAP" >&2

#
# apisix <routes> - APISIX (re)started with these routes; up when it answers at all (502 with no broker)
#
apisix() {
  local config=$GW/apisix-config.yaml
  [ "$1" = ngsild ] && config=$GW_WORK/config-ngsild.yaml
  GW_ROUTES=$GW/routes-$1.yaml GW_APISIX_CONFIG=$config $COMPOSE up -d --force-recreate --no-deps apisix >> "$GW_WORK/compose.log" 2>&1 || die "compose up apisix ($1)"
  await 60 curl -s -o /dev/null http://localhost:9080/ || die "APISIX did not come up ($1) - $COMPOSE logs apisix"
}

#
# Each gateway that decides lets the token through and refuses no token, before anything is measured: a broker
# started for the check, a request through APISIX with the token (the broker's 404) and one without (401)
#
coraine --port $BROKER_PORT --database corDB --troe none > "$GW_WORK/check-broker.log" 2>&1 &
checkPid=$!
letThrough() { [ "$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" http://localhost:9080/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:x)" = 404 ]; }
noToken() { curl -s -o /dev/null -w '%{http_code}' http://localhost:9080/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:x; }
refused() { [ "$(noToken)" = 401 ]; }
for r in opa ngsild; do
  [ $r = ngsild ] && [[ " ${CONFIGS[*]} " != *" plugin "* ]] && continue
  apisix $r
  #
  # No token first: APISIX answers 404 before it has read its routes, which letThrough cannot tell from the
  # broker's - a 401 says the route, and its plugins, are in place
  #
  await 60 refused || { kill $checkPid; die "APISIX ($r) without a token answered $(noToken), not 401"; }
  await 90 letThrough || { kill $checkPid; die "APISIX ($r) does not let the token through - $COMPOSE logs apisix opa"; }
  echo "gatewayCompare.sh: APISIX ($r) lets the token through and refuses no token (401)" >&2
done
kill $checkPid; wait $checkPid 2>/dev/null

#
# cost <label> <loadPort> <token> <brokerArgs> - the CPU each process takes per request: the broker, APISIX and
# OPA each on a CPU of its own, so /proc/stat's busy time of that CPU across a 10 s load, over the requests, is
# what one request costs it. The same broker started again (as perfRun.sh starts it), 100 entities, then a
# retrieve and a PATCH at 50 connections from the load generator's cores. Busy: user + nice + system + irq +
# softirq + steal, in USER_HZ ticks (10 ms: 1 000 per CPU in 10 s)
#
busy() { awk -v c="cpu$1" '$1 == c { print $2 + $3 + $4 + $7 + $8 + $9 }' /proc/stat; }
cost() {
  local label=$1 load=$2 token=$3 args=$4
  local loadCpus; loadCpus=$(IFS=,; echo "${physCpus[*]:3}")
  local authH=(); [ -n "$token" ] && authH=(-H "Authorization: Bearer $token")
  # shellcheck disable=SC2086  # args is an option list
  taskset -c "${physCpus[0]}" coraine --port $BROKER_PORT --database corDB --troe none $args > "$OUT/$label-cost-broker.log" 2>&1 &
  local bpid=$!
  await 30 curl -sf "${authH[@]}" "http://localhost:$load/ngsi-ld/v1/types" || { kill $bpid; echo "gatewayCompare.sh: $label: no broker for the cost" >&2; return 1; }
  python3 -c 'import json; print(json.dumps([{"id": "urn:ngsi-ld:Vehicle:%d" % i, "type": "Vehicle", "speed": {"type": "Property", "value": i}} for i in range(1, 101)]))' \
    | curl -sf -o /dev/null "${authH[@]}" -H 'Content-Type: application/json' --data-binary @- "http://localhost:$load/ngsi-ld/v1/entityOperations/create" \
    || { kill $bpid; echo "gatewayCompare.sh: $label: no fixture for the cost" >&2; return 1; }
  local out="{" first=1 scen
  for scen in retrieve patch; do
    local w=(wrk -t4 -c50 "${authH[@]}")
    [ $scen = patch ] && w+=(-s "$HERE/patchAttr.lua")
    local url="http://localhost:$load/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:7"; [ $scen = patch ] && url="http://localhost:$load"
    PERF_AUTH="$token" PERF_ENTITIES=100 taskset -c "$loadCpus" "${w[@]}" -d2s "$url" > /dev/null 2>&1
    local b0 a0 o0 b1 a1 o1 res
    b0=$(busy "${physCpus[0]}"); a0=$(busy "$GW_APISIX_CPU"); o0=$(busy "$GW_OPA_CPU")
    res=$(PERF_AUTH="$token" PERF_ENTITIES=100 taskset -c "$loadCpus" "${w[@]}" -d10s "$url" 2>&1)
    b1=$(busy "${physCpus[0]}"); a1=$(busy "$GW_APISIX_CPU"); o1=$(busy "$GW_OPA_CPU")
    local reqs bad
    reqs=$(awk '/requests in/ { print $1 }' <<< "$res")
    bad=$(awk '/Non-2xx or 3xx responses:/ { print $NF }' <<< "$res")
    [ -z "$bad" ] || echo "gatewayCompare.sh: $label cost $scen: $bad non-2xx answers" >&2
    [ $first = 1 ] || out+=", "; first=0
    out+=$(python3 -c "import sys; r, *t = map(float, sys.argv[1:]); us = lambda d: round(d * 10000.0 / r, 2); print('"%s": {"requests": %d, "broker_us": %s, "apisix_us": %s, "opa_us": %s, "non2xx": %s}' % ('$scen', r, us(t[0]), us(t[1]), us(t[2]), '${bad:-0}'))" \
      "${reqs:-1}" $((b1 - b0)) $((a1 - a0)) $((o1 - o0)))
  done
  kill $bpid; wait $bpid 2>/dev/null
  echo "$out}" > "$OUT/$label-cost.json"
  echo "gatewayCompare.sh: $label cost: $(cat "$OUT/$label-cost.json")" >&2
}

#
# The configurations
#
ix=0
for c in "${CONFIGS[@]}"; do
  ix=$((ix + 1))
  pgrep -x chrome > /dev/null && die "chrome runs - stopped before $c"
  label=$(printf '%02d-%s' $ix "$c")
  auth=""; args=""; load=$BROKER_PORT
  case "$c" in
    none)   ;;
    fdsc)   auth=$TOKEN
            args="--auth fdsc --fdscJwks $GW_WORK/jwks.json --fdscPolicies $OUT/policies-$N.json --fdscIssuer $VERIFIER --fdscOrganization did:web:provider" ;;
    apisix) apisix plain; load=9080 ;;
    opa)    apisix opa;   load=9080; auth=$TOKEN ;;
    plugin) apisix ngsild; load=9080; auth=$TOKEN ;;
    *)      die "unknown configuration '$c'" ;;
  esac
  echo "=== $(date +%H:%M) $label: perfRun.sh $DB, broker :$BROKER_PORT, load :$load" >&2
  PERF_AUTH="$auth" PERF_LOAD_PORT=$load PERF_BROKER_ARGS="${PERF_BROKER_ARGS:-} $args" \
    "$HERE/perfRun.sh" "$DB" $BROKER_PORT > "$OUT/$label.json" 2> "$OUT/$label.log" \
    || { echo "gatewayCompare.sh: $label failed - $OUT/$label.log" >&2; tail -5 "$OUT/$label.log" >&2; }
  [ "${GW_COST:-1}" = 0 ] || cost "$label" "$load" "$auth" "$args"
done
echo "=== $(date +%H:%M) done" >&2
