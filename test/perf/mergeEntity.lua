--
-- mergeEntity.lua - PATCH /entities/{id}: a merge of two attributes into an entity that exists
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- patchAttr.lua is PATCH /entities/{id}/attrs - an update of attributes that must exist. This is
-- the merge (ETSI "Merge Entity"): the same device-reports-a-value shape, through the other write
-- path, which reads the stored entity, merges the fragment into it and writes the result back.
-- The store keeps its size, so no reset is needed between repeats.
--
local n = tonumber(os.getenv("PERF_ENTITIES")) or 100

math.randomseed(os.time() + tonumber(tostring({}):sub(8)) % 1000)

request = function()
  local id = math.random(1, n)
  return wrk.format("PATCH",
                    "/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:" .. id,
                    {["Content-Type"] = "application/json"},
                    '{"speed":{"type":"Property","value":43,"observedAt":"2026-08-20T10:00:01Z"},' ..
                    '"brand":{"type":"Property","value":"Mercedes-Benz"}}')
end

-- The latency distribution, exact (wrk's own percentile), for perfRun.sh: p50, p95, p99 in microseconds
local function pctl(latency)
  io.write(string.format("PCTL %d %d %d\n", latency:percentile(50), latency:percentile(95), latency:percentile(99)))
end

function done(summary, latency, requests)
  pctl(latency)
end
