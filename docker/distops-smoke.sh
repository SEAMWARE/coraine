#!/usr/bin/env bash
#
# distops-smoke.sh — prove that a forward actually happens, end to end.
#
#   docker compose -f docker/docker-compose-distops.yml up -d
#   ./docker/distops-smoke.sh
#
# Seeds an entity on the Context Source, registers the source on the Broker,
# then asks the Broker for it. The Broker holds no Vehicles of its own, so
# anything it returns can only have come from the forward.
#
# On failure it prints what is needed to tell the five usual causes apart —
# the registration as stored, the NGSILD-Warning header (§ 6.3.5 reports every
# tolerated forward failure there, with the host:port that misbehaved), and
# both brokers' request logs.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail

CB="${CB:-http://localhost:2026}"
CP="${CP:-http://localhost:2027}"
# Log access differs by how the rig was started. With compose installed:
#   COMPOSE="docker compose -f docker/docker-compose-distops.yml"
# Without it (plain `docker run` on a user network), plain container names:
LOGS_CB="${LOGS_CB:-docker logs --tail 40 broker-cb}"
LOGS_CP="${LOGS_CP:-docker logs --tail 40 broker-cp}"

pass=0
fail=0

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32mok\033[0m   %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; fail=$((fail+1)); }

wait_for()
{
  local url="$1" name="$2" i=0
  while [ $i -lt 60 ]; do
    curl -fsS "$url/ngsi-ld/v1/types" >/dev/null 2>&1 && { ok "$name is up"; return 0; }
    i=$((i+1)); sleep 1
  done
  bad "$name never answered at $url"
  return 1
}

say "1. Both brokers reachable"
wait_for "$CP" "broker-cp (source)" || exit 1
wait_for "$CB" "broker-cb (broker)" || exit 1

say "2. Seed a Vehicle on the SOURCE only"
curl -fsS -X POST "$CP/ngsi-ld/v1/entities" \
  -H 'Content-Type: application/json' \
  -d '{"id":"urn:ngsi-ld:Vehicle:D1","type":"Vehicle","speed":{"type":"Property","value":42}}' \
  >/dev/null 2>&1 \
  && ok "seeded urn:ngsi-ld:Vehicle:D1 on broker-cp" \
  || bad "could not seed the source"

# The broker must hold nothing — otherwise a local hit would masquerade as a forward.
localCount=$(curl -fsS "$CB/ngsi-ld/v1/entities?type=Vehicle&local=true" 2>/dev/null | grep -c '"id"')
[ "$localCount" == "0" ] && ok "broker-cb holds no Vehicles locally" \
                         || bad "broker-cb already holds $localCount Vehicle(s) — the test would prove nothing"

say "3. Register the source on the broker"
regStatus=$(curl -fsS -o /tmp/distops-reg.out -w '%{http_code}' -X POST "$CB/ngsi-ld/v1/csourceRegistrations" \
  -H 'Content-Type: application/json' \
  -d '{
    "id": "urn:ngsi-ld:CSR:cp",
    "type": "ContextSourceRegistration",
    "endpoint": "http://broker-cp:1026",
    "mode": "inclusive",
    "operations": ["federationOps"],
    "information": [ { "entities": [ { "type": "Vehicle" } ] } ]
  }' 2>/dev/null)
[ "$regStatus" == "201" ] && ok "registration created (201)" \
                          || bad "registration POST returned $regStatus: $(cat /tmp/distops-reg.out)"

say "4. Ask the broker — the answer can only come from the source"
body=$(curl -fsS -D /tmp/distops-hdr.out "$CB/ngsi-ld/v1/entities?type=Vehicle" 2>/dev/null)
if echo "$body" | grep -q 'urn:ngsi-ld:Vehicle:D1'; then
  ok "the forward worked — broker-cb returned the source's entity"
else
  bad "no forwarded entity came back"
  printf '\n  response body:\n%s\n' "$body"
fi

warn=$(grep -i '^NGSILD-Warning:' /tmp/distops-hdr.out 2>/dev/null)
[ -n "$warn" ] && printf '\n  \033[33m%s\033[0m\n     (§ 6.3.5: a source misbehaved — the host:port above names it)\n' "$warn"

say "5. Retrieve by id forwards too"
byId=$(curl -fsS "$CB/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:D1" 2>/dev/null)
echo "$byId" | grep -q '"speed"' && ok "retrieve-by-id forwarded" \
                                 || bad "retrieve-by-id did not forward: $byId"

if [ $fail -ne 0 ]; then
  say "Diagnostics"
  echo "--- registration as stored:"
  curl -fsS "$CB/ngsi-ld/v1/csourceRegistrations/urn:ngsi-ld:CSR:cp" 2>/dev/null
  echo
  echo "--- broker-cb log (a forward shows as an outgoing request line):"
  $LOGS_CB 2>&1 | tail -40
  echo "--- broker-cp log (did the forward ever arrive?):"
  $LOGS_CP 2>&1 | tail -40
  cat <<'HINT'

If broker-cp's log shows no incoming request, the forward never left broker-cb.
In order of likelihood:
  - --distributed missing on broker-cb (forwarding is off by default)
  - the registration does not match: `type` must match the query exactly, and
    `attributeNames`, when present, narrows it further
  - `operations` does not cover the operation (federationOps is the usual set)
  - the endpoint is not resolvable from inside broker-cb (localhost is itself)
  - cooldown: after one failed forward the endpoint is skipped for
    --cooldownMillis (30s default), behaving as an immediate timeout
  - loop detection: the endpoint resolves to broker-cb's own --httpEndpoint,
    or the request already carries its alias in Via
HINT
fi

say "$pass passed, $fail failed"
[ $fail -eq 0 ]
