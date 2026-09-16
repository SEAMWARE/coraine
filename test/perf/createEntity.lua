--
-- createEntity.lua - POST a NEW entity, every request a different one
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- The other half of the write path. patchAttr.lua measures a device reporting
-- into a store that already knows it; this measures the store learning about
-- something for the first time, which is the more expensive direction: an
-- insert into whatever index the database keeps, not an update in place.
--
-- Every id must be new or the broker answers 409 and the number would be the
-- speed of rejecting duplicates. So each wrk thread owns a disjoint id space -
-- its own slot number - and walks it once. No coordination between threads, and
-- no chance of two of them meeting.
--
-- ⚠️ The store GROWS while this runs, which is why perfRun.sh empties it before
-- every repeat. Without that, repeat 3 starts from a store a thousand times the
-- size repeat 1 did, and the median is of three different experiments.
--
local seq = 0
local head, tail

--
-- ⚠️ NO `local slot` here, and that is the whole trick.
--
-- thread:set() injects a GLOBAL into the thread's Lua state, and a file-scope
-- `local` of the same name shadows it - so every thread read slot 0, every
-- thread generated the same ids, and seven of eight requests came back 409.
-- wrk counts those: the scenario reported 166 036 "creates" per second, which
-- was the speed of refusing duplicates, and it looked like a triumph.
--
local threads = 0
function setup(thread)
  thread:set("threadSlot", threads)
  threads = threads + 1
end

function init(args)
  local slot = threadSlot or 0
  --
  -- The same five-attribute ~550-byte entity as the fixture, so a create is
  -- comparable with the reads and patches measured beside it. Split once,
  -- around the id, so a request is two concatenations rather than a format.
  --
  head = '{"id":"urn:ngsi-ld:Vehicle:new-' .. slot .. '-'
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
                    {["Content-Type"] = "application/json"},
                    head .. seq .. tail)
end
