#!/usr/bin/env python3
#
# perfPolicies.py <n> [p|n] - the policy sets of the perf harnesses (authOverhead.sh, gatewayCompare.sh), as
# one JSON array on stdout:
#
#   p: n-1 odrl:use policies for entity types nothing in the store has (Other1 ...), then allowAll (http:path
#      regex ^/) last - every request checked against all n before one lets it in
#   n: n-2 of those, then endpoints (http:path regex ^/ngsi-ld/v1/(subscriptions|types)) and storeTypes
#      (ngsi-ld:entityType isAnyOf the stores' types) - for --fdscNgsild
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import json, sys
n    = int(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) > 2 else "p"
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
