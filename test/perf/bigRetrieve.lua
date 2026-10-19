--
-- bigRetrieve.lua - GET a RANDOM entity of perfRun.sh's larger store
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- retrieve_c50 reads one fixed entity of a hundred: it measures the request, not the store. This reads a
-- random one of PERF_BIG_ENTITIES, so that the id lookup and the entity's memory (or its document) are
-- the store's size's, not one hot entity's.
--
local auth = os.getenv("PERF_AUTH") and ("Bearer " .. os.getenv("PERF_AUTH"))   -- perfRun.sh PERF_AUTH; nil: no header
local n    = tonumber(os.getenv("PERF_BIG_ENTITIES")) or 300000

-- bigFill names entity i by its type: every 100th is R1, every 1000th + 50 is R01, the rest Common. The
-- same rule here, so that a random i names an entity that is there - a 404 would measure the miss.
local function bigId(i)
  if i % 100 == 0 then return "urn:ngsi-ld:Vehicle:r1-" .. i end
  if i % 1000 == 50 then return "urn:ngsi-ld:Vehicle:r01-" .. i end
  return "urn:ngsi-ld:Vehicle:big-" .. i
end

math.randomseed(os.time() + tonumber(tostring({}):sub(8)) % 1000)

request = function()
  return wrk.format("GET", "/ngsi-ld/v1/entities/" .. bigId(math.random(1, n)), {["Authorization"] = auth})
end

-- The latency distribution, exact (wrk's own percentile), for perfRun.sh: p50, p95, p99 in microseconds
local function pctl(latency)
  io.write(string.format("PCTL %d %d %d\n", latency:percentile(50), latency:percentile(95), latency:percentile(99)))
end

function done(summary, latency, requests)
  pctl(latency)
end
