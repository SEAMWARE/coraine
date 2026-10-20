--
-- bigCreate.lua - POST a NEW entity into perfRun.sh's larger store
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- createEntity.lua's entity, into a store of PERF_BIG_ENTITIES. That store is NOT emptied between
-- repeats - refilling millions of entities per repeat is not affordable - so every wrk run needs ids of
-- its own: createEntity.lua's per-thread slot, and before it a tag made once per wrk run (the clock and a
-- random number - the warm-up and every repeat get their own). The store grows by what the runs create:
-- the rate times the warm-up and the repeats, which perfRun.sh runs last for that reason.
--
local auth = os.getenv("PERF_AUTH") and ("Bearer " .. os.getenv("PERF_AUTH"))   -- perfRun.sh PERF_AUTH; nil: no header

local seq = 0
local head, tail

-- thread:set() injects a GLOBAL - no file-scope `local threadSlot` (see createEntity.lua)
-- The same for runTag: made here, in the main state, once per run - every thread gets the one value
math.randomseed(os.time() + tonumber(tostring({}):sub(8)) % 1000)
local tag     = os.time() .. "-" .. math.random(1, 1000000000)
local threads = 0
function setup(thread)
  thread:set("runTag", tag)
  thread:set("threadSlot", threads)
  threads = threads + 1
end

function init(args)
  local slot = threadSlot or 0
  head = '{"id":"urn:ngsi-ld:Vehicle:bignew-' .. (runTag or "0") .. '-' .. slot .. '-'
  tail = '","type":"Vehicle",' ..
         '"brand":{"type":"Property","value":"Mercedes"},' ..
         '"speed":{"type":"Property","value":42,"observedAt":"2026-08-20T10:00:00Z"},' ..
         '"location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[13.4,52.5]}},' ..
         '"isParked":{"type":"Relationship","object":"urn:ngsi-ld:OffStreetParking:1"},' ..
         '"description":{"type":"Property","value":"a five-attribute vehicle used for throughput measurement, padded to roughly five hundred bytes so the numbers mean something ------------------------------------------------"}}'
end

request = function()
  seq = seq + 1
  return wrk.format("POST", "/ngsi-ld/v1/entities",
                    {["Content-Type"] = "application/json", ["Authorization"] = auth},
                    head .. seq .. tail)
end

-- The latency distribution, exact (wrk's own percentile), for perfRun.sh: p50, p95, p99 in microseconds
local function pctl(latency)
  io.write(string.format("PCTL %d %d %d\n", latency:percentile(50), latency:percentile(95), latency:percentile(99)))
end

function done(summary, latency, requests)
  pctl(latency)
end
