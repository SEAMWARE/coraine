-- latency.lua - for a scenario wrk runs as a plain URL: only the latency distribution perfRun.sh reads

-- The latency distribution, exact (wrk's own percentile), for perfRun.sh: p50, p95, p99 in microseconds
local function pctl(latency)
  io.write(string.format("PCTL %d %d %d\n", latency:percentile(50), latency:percentile(95), latency:percentile(99)))
end

function done(summary, latency, requests)
  pctl(latency)
end
