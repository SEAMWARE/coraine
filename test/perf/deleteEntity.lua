--
-- deleteEntity.lua - DELETE an entity, every request a different one, until this thread's share is gone
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- A delete CONSUMES what it measures: the second DELETE of an id is a 404, and a rate measured over
-- 404s is the speed of saying "not found". So perfRun.sh fills a pool of entities before every
-- repeat - urn:ngsi-ld:Vehicle:del-<n>, n = 1..PERF_POOL - and each wrk thread deletes its own
-- slice of it and then STOPS. wrk divides by the time the threads actually ran, so the rate is
-- what the deletes took - not a fixed duration padded with idle connections.
--
-- ⚠️ wrk builds ONE request before the run starts and never sends it: thread 0's first delete is
-- lost, so one entity of the pool survives every run. The run's count is right regardless - it counts
-- responses, not ids.
--
-- ⚠️ The last requests of a slice are already in flight when its last delete is sent, and wrk will
-- ask for more before the thread stops: those are a GET of an entity that exists (200), never a
-- delete of one that does not. At most a connection's worth per thread - a few among thousands.
--
local pool    = tonumber(os.getenv("PERF_POOL")) or 10000
local nthread = tonumber(os.getenv("PERF_THREADS")) or 1
local from, to, next_id, completed, slice

--
-- ⚠️ thread:set() injects a GLOBAL - no file-scope local may be called threadSlot (see createEntity.lua).
--
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
  local slot = threadSlot or 0
  slice   = math.floor(pool / nthread)
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
  local id = next_id
  next_id = next_id + 1
  return wrk.format("DELETE", "/ngsi-ld/v1/entities/urn:ngsi-ld:Vehicle:del-" .. id)
end

response = function(status, headers, body)
  completed = completed + 1
  if completed <= slice then tLast = nowUs() end
  if completed >= slice then
    wrk.thread:set("tFirst", tFirst)
    wrk.thread:set("tLast", tLast)
    wrk.thread:set("ops", completed)
    wrk.thread:stop()
  end
end

function done(summary, latency, requests)
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
