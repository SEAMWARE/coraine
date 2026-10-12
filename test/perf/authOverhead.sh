#!/usr/bin/env bash
#
# authOverhead.sh - what the auth plugin costs: perfRun.sh without --auth, and with --auth fdsc and 1, 10
# and 20 policies, the same build, the same store, one JSON object per configuration in <outDir>.
#
# Usage:  authOverhead.sh <outDir> [none|p1|p10|p20 ...]      (default: all four)
#
# The token: one RS256 token (tools/ftJwt.py), valid for a day, a UserCredential - every request carries
# it (PERF_AUTH). The policies: N-1 that never hold for the measured requests, and allowAll last, so each
# request is checked against all N before one lets it in - the most a set of N can cost. The N-1 are
# odrl:use (every method: the method mask skips none of them) on an entity type nothing in the store has,
# the per-type policy a data space deploys.
#
# Everything else is perfRun.sh's: PERF_BROKER_CORES (default 1 here - the number per core), PERF_DB
# (default corDB), PERF_REPEATS, PERF_DURATION, ...
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

OUT=${1:?usage: authOverhead.sh <outDir> [none|p1|p10|p20 ...]}
shift
CONFIGS=( "${@:-none p1 p10 p20}" )
read -r -a CONFIGS <<< "${CONFIGS[*]}"

SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
JWT="$SCRIPTDIR/../funcTests/tools/ftJwt.py"
DB=${PERF_DB:-corDB}
export PERF_BROKER_CORES=${PERF_BROKER_CORES:-1}

mkdir -p "$OUT"
KEYS="$OUT/keys"
mkdir -p "$KEYS"

python3 "$JWT" key  --out "$KEYS" --name perf
python3 "$JWT" jwks --out "$KEYS/jwks.json" "$KEYS/perf.jwk"
TOKEN=$(python3 "$JWT" sign --key "$KEYS/perf.pem" --exp 86400 \
          --vc '{"type": ["VerifiableCredential", "UserCredential"], "issuer": "did:web:consumer", "credentialSubject": {}}')

#
# policies <n> - n policies, allowAll last
#
policies() {
  python3 - "$1" <<'EOF'
import json, sys
n = int(sys.argv[1])
ctx = {"odrl": "http://www.w3.org/ns/odrl/2/"}
def constraint(left, op, right):
    return {"@type": "odrl:Constraint", "odrl:leftOperand": left, "odrl:operator": {"@id": op}, "odrl:rightOperand": right}
out = []
for k in range(1, n):
    out.append({"@context": ctx, "@type": "odrl:Policy", "odrl:uid": "urn:perf:policy:other%d" % k,
                "odrl:permission": {"odrl:assignee": {"@id": "vc:any"}, "odrl:action": {"@id": "odrl:use"},
                                    "odrl:target": {"@type": "odrl:AssetCollection",
                                                    "odrl:refinement": [constraint("ngsi-ld:entityType", "odrl:eq", "Other%d" % k)]}}})
out.append({"@context": ctx, "@type": "odrl:Policy", "odrl:uid": "urn:perf:policy:allowAll",
            "odrl:permission": {"odrl:assignee": {"@id": "vc:any"}, "odrl:action": {"@id": "odrl:use"},
                                "odrl:target": {"@type": "odrl:AssetCollection", "odrl:source": "urn:asset",
                                                "odrl:refinement": [constraint("http:path", "odrl:regex", "^/")]}}})
print(json.dumps(out, indent=1))
EOF
}

for c in "${CONFIGS[@]}"; do
  case "$c" in
    none) auth=""; args="" ;;
    p[0-9]*)
      n=${c#p}
      policies "$n" > "$OUT/policies-$n.json"
      auth=$TOKEN
      args="--auth fdsc --fdscJwks $KEYS/jwks.json --fdscPolicies $OUT/policies-$n.json --fdscIssuer http://verifier.ft --fdscOrganization did:web:provider"
      ;;
    *) echo "authOverhead.sh: unknown configuration '$c'" >&2; exit 1 ;;
  esac

  echo "authOverhead.sh: $c - perfRun.sh $DB, broker on $PERF_BROKER_CORES core(s)" >&2
  PERF_AUTH="$auth" PERF_BROKER_ARGS="${PERF_BROKER_ARGS:-} $args" "$SCRIPTDIR/perfRun.sh" "$DB" > "$OUT/$c.json"
done

#
# The table: every rate beside the run without auth
#
python3 - "$OUT" "${CONFIGS[@]}" <<'EOF'
import json, os, sys
out, configs = sys.argv[1], sys.argv[2:]
runs = {c: json.load(open(os.path.join(out, c + ".json"))) for c in configs}
keys = [k for k in runs[configs[0]] if k != "db" and not k.endswith("us")]
print("%-26s" % "scenario" + "".join("%14s" % c for c in configs))
for k in keys:
    row = "%-26s" % k
    base = runs.get("none", {}).get(k)
    for c in configs:
        v = runs[c].get(k)
        if base and c != "none" and v is not None:
            row += "%14s" % ("%d (%+.1f%%)" % (v, 100.0 * (v - base) / base))
        else:
            row += "%14s" % v
    print(row)
EOF
