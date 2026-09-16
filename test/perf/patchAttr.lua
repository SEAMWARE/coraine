--
-- patchAttr.lua - PATCH an attribute of a RANDOM existing entity
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- The characteristic write of a context broker: a device reports a new value
-- for an entity that already exists. Random id per request so the number is the
-- average cost rather than whatever one fixed entity's position happens to be,
-- and because hammering ONE entity measures lock contention on that entity
-- instead of the write path.
--
-- The store does not grow during the run - every request updates an entity that
-- is already there - so the measurement is stable and repeatable, which a
-- create benchmark is not.
--
local n = tonumber(os.getenv("PERF_ENTITIES")) or 100

math.randomseed(os.time() + tonumber(tostring({}):sub(8)) % 1000)

request = function()
  local id = math.random(1, n)
  return wrk.format("PATCH",
                    "/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:" .. id .. "/attrs",
                    {["Content-Type"] = "application/json"},
                    '{"speed":{"type":"Property","value":42,"observedAt":"2026-08-20T10:00:00Z"}}')
end
