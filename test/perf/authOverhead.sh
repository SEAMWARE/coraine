#!/usr/bin/env bash
#
# authOverhead.sh - what the auth plugin costs: perfRun.sh without --auth, without --auth but every request
# carrying the token (what receiving it costs), and with --auth fdsc and 1, 10 and 20 policies; the same
# build, the same store, one JSON object per configuration in <outDir>.
#
# Usage:  authOverhead.sh <outDir> [none|token|pN|nN ...]      (default: none token p1 p10 p20)
#
#   nN: --fdscNgsild (decided on each entity, notifications for their owner) and N policies: N-2 for types
#       nothing has, one http:path policy for /subscriptions and /types, and last the one that allows -
#       ngsi-ld:entityType isAnyOf the store's types - so every entity is decided on its stored type
#
# A configuration may be given twice (none ... none): a run's last configurations read deep pages low, and the
# second run of the same one says by how much.
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

OUT=${1:?usage: authOverhead.sh <outDir> [none|token|pN|nN ...]}
shift
CONFIGS=( "${@:-none token p1 p10 p20}" )
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
  python3 - "$1" "${2:-p}" <<'EOF'
import json, sys
n    = int(sys.argv[1])
mode = sys.argv[2]
ctx = {"odrl": "http://www.w3.org/ns/odrl/2/"}
def constraint(left, op, right):
    return {"@type": "odrl:Constraint", "odrl:leftOperand": left, "odrl:operator": {"@id": op}, "odrl:rightOperand": right}
out = []
if mode == "n":
    for k in range(1, n - 1):
        out.append({"@context": ctx, "@type": "odrl:Policy", "odrl:uid": "urn:perf:policy:other%d" % k,
                    "odrl:permission": {"odrl:assignee": {"@id": "vc:any"}, "odrl:action": {"@id": "odrl:use"},
                                        "odrl:target": {"@type": "odrl:AssetCollection",
                                                        "odrl:refinement": [constraint("ngsi-ld:entityType", "odrl:eq", "Other%d" % k)]}}})
    out.append({"@context": ctx, "@type": "odrl:Policy", "odrl:uid": "urn:perf:policy:endpoints",
                "odrl:permission": {"odrl:assignee": {"@id": "vc:any"}, "odrl:action": {"@id": "odrl:use"},
                                    "odrl:target": {"@type": "odrl:AssetCollection", "odrl:source": "urn:asset",
                                                    "odrl:refinement": [constraint("http:path", "odrl:regex", "^/ngsi-ld/v1/(subscriptions|types)")]}}})
    out.append({"@context": ctx, "@type": "odrl:Policy", "odrl:uid": "urn:perf:policy:storeTypes",
                "odrl:permission": {"odrl:assignee": {"@id": "vc:any"}, "odrl:action": {"@id": "odrl:use"},
                                    "odrl:target": {"@type": "odrl:AssetCollection",
                                                    "odrl:refinement": [constraint("ngsi-ld:entityType", "odrl:isAnyOf", ["Vehicle", "Common", "Rare", "R1", "R01"])]}}})
    print(json.dumps(out, indent=1))
    sys.exit(0)
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

ix=0
for c in "${CONFIGS[@]}"; do
  ix=$((ix + 1))
  label=$c; [ -e "$OUT/$c.json" ] && label="$c.$ix"     # a configuration given twice: its second file
  case "$c" in
    none)  auth="";     args="" ;;
    token) auth=$TOKEN; args="" ;;               # no plugin: the header is received and never read
    p[0-9]*)
      n=${c#p}
      policies "$n" > "$OUT/policies-$n.json"
      auth=$TOKEN
      args="--auth fdsc --fdscJwks $KEYS/jwks.json --fdscPolicies $OUT/policies-$n.json --fdscIssuer http://verifier.ft --fdscOrganization did:web:provider"
      ;;
    n[0-9]*)
      n=${c#n}
      policies "$n" n > "$OUT/policies-n$n.json"
      auth=$TOKEN
      args="--auth fdsc --fdscNgsild --fdscJwks $KEYS/jwks.json --fdscPolicies $OUT/policies-n$n.json --fdscIssuer http://verifier.ft --fdscOrganization did:web:provider"
      ;;
    *) echo "authOverhead.sh: unknown configuration '$c'" >&2; exit 1 ;;
  esac

  echo "authOverhead.sh: $c - perfRun.sh $DB, broker on $PERF_BROKER_CORES core(s)" >&2
  PERF_AUTH="$auth" PERF_BROKER_ARGS="${PERF_BROKER_ARGS:-} $args" "$SCRIPTDIR/perfRun.sh" "$DB" > "$OUT/$label.json"
done

#
# The table: every rate beside the run without auth
#
python3 - "$OUT" <<'EOF'
import json, os, sys, glob
out = sys.argv[1]
configs = sorted([os.path.basename(f)[:-5] for f in glob.glob(os.path.join(out, "*.json")) if not os.path.basename(f).startswith("policies")],
                 key=lambda c: os.path.getmtime(os.path.join(out, c + ".json")))
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
