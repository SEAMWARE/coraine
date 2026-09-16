--
-- batchUpdate.lua - update PERF_BATCH entities in ONE request
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- The write-side counterpart of query(limit=20) against query(limit=1): the
-- same work, arriving in one request instead of twenty. Per ENTITY it should be
-- far cheaper, because one HTTP request, one parse of the URL params, one
-- @context resolution and one lock acquisition are amortised over the batch
-- instead of paid twenty times.
--
-- Compare `batch20_c50 x 20` against `patch_c50` - both are entities per second
-- and the ratio is what batching is worth. If it is near 1, the per-request
-- overhead is not where the time goes.
--
-- The body is built ONCE, at load: rebuilding it per request would measure the
-- load generator's string handling rather than the broker's write path.
--
local batch = tonumber(os.getenv("PERF_BATCH")) or 20

local parts = {}
for i = 1, batch do
  parts[#parts + 1] = string.format(
    '{"id":"urn:ngsi-ld:Vehicle:%d","type":"Vehicle",' ..
    '"speed":{"type":"Property","value":42,"observedAt":"2026-08-20T10:00:00Z"}}', i)
end
local body = "[" .. table.concat(parts, ",") .. "]"

request = function()
  return wrk.format("POST", "/ngsi-ld/v1/entityOperations/update",
                    {["Content-Type"] = "application/json"}, body)
end
