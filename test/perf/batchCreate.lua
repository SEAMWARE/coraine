--
-- batchCreate.lua - create PERF_BATCH NEW entities in ONE request
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- batchUpdate.lua against patchAttr.lua says what batching is worth on the
-- update path. This is the same question on the create path, where it should
-- matter more: an update finds an entity that is already there, a create has to
-- put it somewhere, and doing twenty of those under one lock acquisition and one
-- @context resolution is a bigger saving than doing twenty updates that way.
--
-- Compare `batch20create_c50 x 20` against `create_c50`, both entities/s.
--
-- Unlike batchUpdate.lua the body CANNOT be built once: every id has to be new,
-- so it is rebuilt per request. That work is the load generator's, not the
-- broker's, so this number is a floor - the broker is at least this fast. If
-- the ratio against create_c50 ever comes out near 1, suspect the generator
-- before concluding batching is worthless, and check whether wrk's own cores
-- are saturated.
--
local batch = tonumber(os.getenv("PERF_BATCH")) or 20
local base  = 0
local head, tail

--
-- ⚠️ thread:set() injects a GLOBAL, so nothing here may declare `slot` as a
-- file-scope local - a local of the same name shadows it, every thread reads
-- slot 0, every thread builds the same ids and the broker answers 409 to all
-- but one of them. wrk counts those, so the scenario reports a number.
--
local threads = 0
function setup(thread)
  thread:set("threadSlot", threads)
  threads = threads + 1
end

function init(args)
  local slot = threadSlot or 0
  head = '{"id":"urn:ngsi-ld:Vehicle:new-' .. slot .. '-'
  tail = '","type":"Vehicle",' ..
         '"brand":{"type":"Property","value":"Mercedes"},' ..
         '"speed":{"type":"Property","value":42,"observedAt":"2026-08-20T10:00:00Z"},' ..
         '"location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[13.4,52.5]}},' ..
         '"isParked":{"type":"Relationship","object":"urn:ngsi-ld:OffStreetParking:1"},' ..
         '"description":{"type":"Property","value":"a five-attribute vehicle used for throughput measurement, padded to roughly five hundred bytes so the numbers mean something ------------------------------------------------"}}'
end

local parts = {}
request = function()
  for i = 1, batch do
    parts[i] = head .. (base + i) .. tail
  end
  base = base + batch
  return wrk.format("POST", "/ngsi-ld/v1/entityOperations/create",
                    {["Content-Type"] = "application/json"},
                    "[" .. table.concat(parts, ",", 1, batch) .. "]")
end
