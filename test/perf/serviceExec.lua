--
-- serviceExec.lua - run a service on a RANDOM existing entity (sePerf.sh)
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- POST /ngsi-ld/v1/entities/{id}/services/{name}: PERF_SE_SERVICE names the service - "svc" (synchronous:
-- 200, the output) or "act" (asynchronous: 202, the execution followed by its reports). A random entity per
-- request, as patchAttr.lua, so the number is the average cost and not one entity's lock.
--
local auth = os.getenv("PERF_AUTH") and ("Bearer " .. os.getenv("PERF_AUTH"))
local n    = tonumber(os.getenv("PERF_ENTITIES")) or 100
local svc  = os.getenv("PERF_SE_SERVICE") or "svc"

math.randomseed(os.time() + tonumber(tostring({}):sub(8)) % 1000)

request = function()
  local id = math.random(1, n)
  return wrk.format("POST",
                    "/ngsi-ld/v1/entities/urn:ngsi-ld:Robot:" .. id .. "/services/" .. svc,
                    {["Content-Type"] = "application/json", ["Authorization"] = auth},
                    '{"speed":0.5,"target":"dock"}')
end

function done(summary, latency, requests)
  io.write(string.format("PCTL %d %d %d\n", latency:percentile(50), latency:percentile(95), latency:percentile(99)))
end
