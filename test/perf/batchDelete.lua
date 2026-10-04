--
-- batchDelete.lua - delete PERF_BATCH entities in ONE request, until this thread's share is gone
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- deleteEntity.lua in batches: POST /entityOperations/delete with twenty ids. The same pool
-- (urn:ngsi-ld:Vehicle:del-<n>, filled by perfRun.sh before every repeat), the same per-thread
-- slices and the same stop when a slice is gone - see deleteEntity.lua for why.
--
-- ⚠️ wrk builds ONE request before the run starts and never sends it: thread 0's first batch is
-- lost, so one batch of the pool survives every run (plus the ids a pool not divisible into whole
-- batches per thread leaves). The run's count is right regardless - it counts responses, not ids.
--
-- Compare `batch20delete_c50 x 20` against `delete_c50`, both entities/s: what batching is worth
-- on the way out.
--
local pool    = tonumber(os.getenv("PERF_POOL")) or 10000
local nthread = tonumber(os.getenv("PERF_THREADS")) or 1
local batch   = tonumber(os.getenv("PERF_BATCH")) or 20
local from, to, next_id, completed, batches

--
-- ⚠️ wrk's rate is requests / the -d it was given: its main thread sleeps for -d whatever the
-- workers do, so a thread that stops early does not shorten it (5 000 deletes "in 1.00m" = 83/s).
-- So the time is taken HERE, with the monotonic clock: each thread notes its first request and its
-- last response, and done() prints the ops over the real span - first to last, across all threads
-- - as "Consumed/sec:", which perfRun.sh reads instead of wrk's Requests/sec.
--
local ffi = require("ffi")
ffi.cdef[[
  typedef struct { long tv_sec; long tv_nsec; } timespec_t;
  int clock_gettime(int clk, timespec_t* tp);
]]
local tsp = ffi.new("timespec_t[1]")
local function nowUs()
  ffi.C.clock_gettime(1, tsp)                         -- CLOCK_MONOTONIC
  return tonumber(tsp[0].tv_sec) * 1000000 + tonumber(tsp[0].tv_nsec) / 1000
end

local tFirst, tLast = 0, 0
local threads = 0
local threadV = {}
function setup(thread)
  thread:set("threadSlot", threads)
  threadV[#threadV + 1] = thread
  threads = threads + 1
end

function init(args)
  local slot  = threadSlot or 0
  local slice = math.floor(pool / nthread / batch) * batch   -- whole batches only
  batches = slice / batch
  from    = slot * slice + 1
  to      = from + slice - 1
  next_id = from
  completed = 0
end

request = function()
  if tFirst == 0 then tFirst = nowUs() end
  if next_id > to then
    return wrk.format("GET", "/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:1")
  end
  local ids = {}
  for i = 0, batch - 1 do
    ids[#ids + 1] = '"urn:ngsi-ld:Vehicle:del-' .. (next_id + i) .. '"'
  end
  next_id = next_id + batch
  return wrk.format("POST", "/ngsi-ld/v1/entityOperations/delete",
                    {["Content-Type"] = "application/json"},
                    "[" .. table.concat(ids, ",") .. "]")
end

response = function(status, headers, body)
  completed = completed + 1
  if completed <= batches then tLast = nowUs() end
  if completed >= batches then
    wrk.thread:set("tFirst", tFirst)
    wrk.thread:set("tLast", tLast)
    wrk.thread:set("ops", completed)
    wrk.thread:stop()
  end
end

-- The latency distribution, exact (wrk's own percentile), for perfRun.sh: p50, p95, p99 in microseconds
local function pctl(latency)
  io.write(string.format("PCTL %d %d %d\n", latency:percentile(50), latency:percentile(95), latency:percentile(99)))
end

function done(summary, latency, requests)
  pctl(latency)
  local first, last, ops = nil, 0, 0
  for _, t in ipairs(threadV) do
    local f, l, n = t:get("tFirst"), t:get("tLast"), t:get("ops")
    if f and l and n and f > 0 then
      if first == nil or f < first then first = f end
      if l > last then last = l end
      ops = ops + n
    end
  end
  if first ~= nil and last > first then
    print(string.format("Consumed/sec: %.2f", ops / ((last - first) / 1000000)))
    io.stdout:flush()
  end
end
