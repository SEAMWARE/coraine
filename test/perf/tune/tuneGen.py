#!/usr/bin/env python3
#
# tuneGen.py <workload.json> <outDir> - what `make tune` runs against the broker, from the workload file
#
# The workload file describes a deployment: its store, what its entities look like and how many there
# are, its subscriptions, and the mix of requests it serves (doc/extreme-performance.md). From it:
#
#   outDir/fixture.ndjson   the entities, as batch creates of 100 - one JSON array per line
#   outDir/subs.ndjson      the subscriptions - one per line; the notification endpoint is __RECEIVER__
#   outDir/mix.lua          one wrk script: each request drawn from the mix (patch, query, retrieve, create)
#   outDir/broker.args      the store's broker options (--database, --dbDir, --troe)
#
# Ids: urn:ngsi-ld:<type>:<n>, n from 1 per type. Created entities (the mix's "create") take ids no
# fixture entity has - urn:ngsi-ld:<type>:new-<run>-<thread>-<seq> - so a create never answers 409.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import json
import os
import sys


def fail(msg):
    sys.exit("tuneGen: " + msg)


def attrValue(a, n):
    """An attribute's API form - every entity n gets its own value where the type has one."""
    t = a.get("type", "Property")
    if t == "Property":
        v = a.get("value", 0)
        out = {"type": "Property", "value": (v + n % 100) if isinstance(v, (int, float)) and not isinstance(v, bool) else v}
        if a.get("observedAt"):
            out["observedAt"] = "2026-01-01T00:00:00Z"
        for s in range(int(a.get("subAttributes", 0))):
            out["sub%d" % (s + 1)] = {"type": "Property", "value": "s%d" % (s + 1)}
        return out
    if t == "GeoProperty":
        return {"type": "GeoProperty", "value": {"type": "Point", "coordinates": [13.4 + (n % 100) / 1000, 52.5]}}
    if t == "Relationship":
        return {"type": "Relationship", "object": a.get("object", "urn:ngsi-ld:Thing:%d" % n)}
    if t == "LanguageProperty":
        return {"type": "LanguageProperty", "languageMap": a.get("languageMap", {"en": "thing %d" % n})}
    if t == "VocabProperty":
        return {"type": "VocabProperty", "vocab": a.get("vocab", "active")}
    if t == "ListProperty":
        return {"type": "ListProperty", "valueList": a.get("valueList", [1, 2, 3])}
    if t == "JsonProperty":
        return {"type": "JsonProperty", "json": a.get("json", {"n": n})}
    fail("attribute '%s': unknown type '%s'" % (a.get("name"), t))


def main():
    if len(sys.argv) != 3:
        fail("usage: tuneGen.py <workload.json> <outDir>")

    w = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)

    #
    # The store
    #
    store = w.get("store", {})
    db = store.get("database", "corDB")
    if db not in ("corDB", "mongoc"):
        fail("store.database: corDB or mongoc")
    args = ["--database", db]
    if db == "corDB" and store.get("persistent", False):
        args += ["--dbDir", "__DBDIR__"]
    hist = store.get("history", "none")
    if hist not in ("none", "corDB", "timescale"):
        fail("store.history: none, corDB or timescale")
    args += ["--troe", hist]
    open(os.path.join(out, "broker.args"), "w").write(" ".join(args) + "\n")

    #
    # The entities - their count split over the types by share
    #
    ents = w.get("entities", {})
    count = int(ents.get("count", 0))
    types = ents.get("types", [])
    if count <= 0 or not types:
        fail("entities: a count and at least one type")
    shares = [float(t.get("share", 1.0 / len(types))) for t in types]
    total = sum(shares)
    perType = [max(1, round(count * s / total)) for s in shares]

    with open(os.path.join(out, "fixture.ndjson"), "w") as f:
        batch = []
        for t, n in zip(types, perType):
            for i in range(1, n + 1):
                e = {"id": "urn:ngsi-ld:%s:%d" % (t["type"], i), "type": t["type"]}
                for a in t.get("attributes", []):
                    if not isinstance(a, dict) or "name" not in a:
                        fail("type '%s': every attribute an object with a name" % t["type"])
                    e[a["name"]] = attrValue(a, i)
                batch.append(e)
                if len(batch) == 100:
                    f.write(json.dumps(batch) + "\n")
                    batch = []
        if batch:
            f.write(json.dumps(batch) + "\n")

    #
    # The subscriptions
    #
    with open(os.path.join(out, "subs.ndjson"), "w") as f:
        k = 0
        for s in w.get("subscriptions", []):
            sel = s.get("select", "byType")
            for j in range(int(s.get("count", 1))):
                k += 1
                body = {"id": "urn:ngsi-ld:Subscription:tune-%d" % k, "type": "Subscription",
                        "notification": {"endpoint": {"uri": "__RECEIVER__"}}}
                if sel == "perEntity":
                    # one entity each, of one type: the subscription's, or the first - entity 1, 2, 3, ...
                    tIx = next((ix for ix, t in enumerate(types) if t["type"] == s.get("type")), 0)
                    t, n = types[tIx], perType[tIx]
                    body["entities"] = [{"id": "urn:ngsi-ld:%s:%d" % (t["type"], 1 + j % n), "type": t["type"]}]
                elif sel == "byType":
                    body["entities"] = [{"type": s.get("type", types[0]["type"])}]
                else:
                    fail("subscriptions: select is perEntity or byType")
                if s.get("watchedAttributes"):
                    body["watchedAttributes"] = s["watchedAttributes"]
                if s.get("q"):
                    body["q"] = s["q"]
                f.write(json.dumps(body) + "\n")

    #
    # The mix - one wrk script
    #
    load = w.get("load", {})
    mix = load.get("mix", {"patch": 1.0})
    for op in mix:
        if op not in ("patch", "query", "retrieve", "create"):
            fail("load.mix: patch, query, retrieve, create - not '%s'" % op)
    ops = [(op, float(p)) for op, p in mix.items() if float(p) > 0]
    acc, cuts = 0.0, []
    for op, p in ops:
        acc += p
        cuts.append((op, acc))

    patchAttrs = load.get("patch", {}).get("attributes")
    q = load.get("query", {})
    qType = q.get("type", types[0]["type"])
    qs = "type=" + qType + "&limit=" + str(q.get("limit", 20)) + (("&q=" + q["q"]) if q.get("q") else "")

    # The patch body per type: the attributes the mix patches (or the type's first Property)
    patchBody = {}
    for t in types:
        # the attributes the mix patches that this type has - else its first Property
        byName = {a["name"]: a for a in t.get("attributes", [])}
        names  = [nm for nm in (patchAttrs or []) if nm in byName]
        if not names:
            names = [a["name"] for a in t.get("attributes", []) if a.get("type", "Property") == "Property"][:1]
        if not names:
            fail("type '%s': nothing to patch - no Property" % t["type"])
        body = {nm: attrValue(byName.get(nm, {"name": nm, "type": "Property", "value": 1}), 7) for nm in names}
        patchBody[t["type"]] = json.dumps(body)

    lua = []
    lua.append("-- mix.lua - GENERATED by tuneGen.py from the workload file - do not edit")
    lua.append("local types = {" + ", ".join('{t="%s", n=%d}' % (t["type"], n) for t, n in zip(types, perType)) + "}")
    lua.append("local patchBody = {" + ", ".join('["%s"]=%s' % (t, json.dumps(b)) for t, b in patchBody.items()) + "}")
    lua.append("local cuts = {" + ", ".join('{op="%s", c=%f}' % (op, c / acc) for op, c in cuts) + "}")
    lua.append('local query = "/ngsi-ld/v1/entities?%s"' % qs.replace(">", "%3E").replace("<", "%3C").replace("\"", "%22"))
    lua.append('local H = {["Content-Type"] = "application/json"}')
    lua.append("local seq = 0")
    lua.append("")
    lua.append("-- thread:set() injects 'slot' - a global: no local of that name (createEntity.lua says why)")
    lua.append("local counter = 0")
    lua.append("local threads = {}")
    lua.append("function setup(thread) counter = counter + 1; thread:set('slot', counter); table.insert(threads, thread) end")
    lua.append("-- run: the tag of this wrk run (tuneRun.sh passes '-- w' for the warm-up, '-- m' for the measured run) - the")
    lua.append("-- ids a create makes include it, so the measured run never re-creates what the warm-up created (409)")
    lua.append("local run = 'r'")
    lua.append("function init(args) math.randomseed(os.time() + (slot or 0)); run = args[1] or 'r' end")
    lua.append("")
    lua.append("local function pick() local t = types[math.random(1, #types)]; return t, math.random(1, t.n) end")
    lua.append("")
    lua.append("request = function()")
    lua.append("  local r, op = math.random(), cuts[#cuts].op")
    lua.append("  for _, c in ipairs(cuts) do if r <= c.c then op = c.op; break end end")
    lua.append("  local t, n = pick()")
    lua.append('  if op == "patch" then')
    lua.append('    return wrk.format("PATCH", "/ngsi-ld/v1/entities/urn:ngsi-ld:" .. t.t .. ":" .. n .. "/attrs", H, patchBody[t.t])')
    lua.append('  elseif op == "retrieve" then')
    lua.append('    return wrk.format("GET", "/ngsi-ld/v1/entities/urn:ngsi-ld:" .. t.t .. ":" .. n)')
    lua.append('  elseif op == "query" then')
    lua.append('    return wrk.format("GET", query)')
    lua.append("  else")
    lua.append("    seq = seq + 1")
    lua.append('    local id = "urn:ngsi-ld:" .. t.t .. ":new-" .. run .. "-" .. (slot or 0) .. "-" .. seq')
    lua.append('    return wrk.format("POST", "/ngsi-ld/v1/entities", H, \'{"id":"\' .. id .. \'","type":"\' .. t.t .. \'"}\')')
    lua.append("  end")
    lua.append("end")
    lua.append("")
    lua.append("-- the first failure, per thread (a GLOBAL of the thread's state - done() reads it with thread:get):")
    lua.append("-- a run that fails says WHAT failed (tuneRun.sh prints it)")
    lua.append("function response(status, headers, body)")
    lua.append("  if status >= 300 and firstBad == nil then firstBad = status .. ' ' .. (body or ''):sub(1, 300) end")
    lua.append("end")
    lua.append("")
    lua.append("function done(summary, latency, requests)")
    lua.append('  io.write(string.format("PCTL %d %d %d\\n", latency:percentile(50), latency:percentile(95), latency:percentile(99)))')
    lua.append("  for _, t in ipairs(threads) do")
    lua.append("    local b = t:get('firstBad')")
    lua.append('    if b ~= nil then io.write("FIRSTBAD " .. b:gsub("\\n", " ") .. "\\n"); break end')
    lua.append("  end")
    lua.append("end")
    open(os.path.join(out, "mix.lua"), "w").write("\n".join(lua) + "\n")

    print("tuneGen: %d entities over %d types, %d subscriptions, mix %s" % (sum(perType), len(types), k, ", ".join("%s %.0f %%" % (op, p / acc * 100) for op, p in ops)))


main()
