#!/usr/bin/env python3
#
# perfCompare.py - compare today's numbers with the recorded history.
#
# Shared runners are noisy: the same commit can differ by 20-30% between runs. So
# the comparison is against the MEDIAN of the last N recorded runs, not against
# the previous one, and the thresholds are deliberately wide. A perf gate that
# cries wolf is a perf gate nobody reads - the same lesson as a build full of
# warnings.
#
# Reads:  history file (one JSON object per line), today's JSON on argv
# Writes: a markdown summary on stdout; exit 1 only on a collapse
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import json, os, re, sys, statistics

WARN_PCT, FAIL_PCT, WINDOW = 20.0, 50.0, 5


#
# ⭐ Not every metric is "bigger is better".
#
# The throughput metrics are requests/second - up is good. The `_p50us`,
# `_p95us` and `_p99us` ones are LATENCY percentiles in microseconds - down is
# good. Comparing them the same way
# gets the answer exactly backwards: a runner that happens to be fast raises
# every throughput number AND lowers every p99, and the p99 rows then read as
# a collapse. That is not hypothetical - it is why this job went red on
# 2026-09-18 and 2026-09-19, on runs where the broker was 80-180% FASTER than
# the median on every throughput metric.
#
# The dangerous half is the other direction: with the sign unhandled, a real
# doubling of p99 latency reads as a +100% improvement and the gate stays
# green. A perf gate that is loud when things improve and silent when they
# rot is worse than no gate.
#
# Every percentile, not just p99: perfRun.sh has reported p50 and p95 beside
# p99 since 2026-10-04 (7ae40963), and with only `_p99us` matched here they
# were read as throughput. The release/0.5.0 nightly went red on exactly that -
# the rare-type and big-store queries got 10-150x faster (#300) and their p50
# and p95 rows read as -60 % to -99 % collapses.
#
def lowerIsBetter(metric):
    return re.search(r"_p\d+us$", metric) is not None

#
# cpu - the machine this run is on, as perfRecord.py records it
#
# ⭐ The history is filtered to it. GitHub's shared runners come in more than one CPU generation, and
# every metric of both databases moves together by 30-40% from one to the other (2026-10-01 ->
# 10-02: +100% everywhere; 10-03: -30% everywhere, on a commit that changed no hot path). Compared
# across machines, the gate measured the draw of the runner. Records from before the field existed
# have no machine, and are not compared with.
#
def cpu():
    model = "unknown"
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                model = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return f"{model} x{os.cpu_count()}"

historyFile, todayJson = sys.argv[1], sys.argv[2]
today   = json.loads(todayJson)
db      = today["db"]
machine = cpu()

history = []
try:
    for line in open(historyFile):
        line = line.strip()
        if line:
            rec = json.loads(line)
            if (rec.get("db") == db) and (rec.get("cpu") == machine):
                history.append(rec)
except FileNotFoundError:
    pass

metrics = [k for k in today if k not in ("db", "date", "sha", "run", "cpu")]
print(f"### Performance — `{db}`\n")
print(f"On `{machine}` - compared with the {min(len(history), WINDOW)} latest runs on the same CPU.\n")
print("| metric | now | median of last %d | change | | " % WINDOW)
print("|---|---:|---:|---:|---|")

#
# ⭐ A tail percentile (p95, p99) fails the gate only when its scenario moved too.
#
# A real regression moves the whole scenario: throughput drops, p50 rises, the tail with them. A
# p99 on its own is the runner's tail - on a ~120 us single-connection p99, 60 us of scheduling
# noise is "+50 %". The release/0.5.0 nightly (2026-10-09) went red on corDB create_c1_p99us
# 126 -> 201 us while that scenario's throughput was +2.4 % and its p50 and p95 sat in their
# usual range. So a p95/p99 past the fail line counts only when the same scenario's throughput
# or p50 is past the warn line as well; otherwise it is shown as a warning, "tail only".
#
def scenarioOf(m):
    return re.sub(r"_p\d+us$", "", m)

def isTail(m):
    return re.search(r"_p\d+us$", m) is not None and not m.endswith("_p50us")

rows = []
for m in metrics:
    now  = today[m]
    past = [r[m] for r in history[-WINDOW:] if m in r]
    if not past:
        rows.append((m, now, None, None, None))
        continue
    ref    = statistics.median(past)
    change = (now - ref) / ref * 100.0
    # `delta` is the change expressed as "better or worse", whichever way the
    # metric runs. Every threshold below is on delta; the table still prints
    # the true change, so the numbers can be checked against the raw records.
    delta  = -change if lowerIsBetter(m) else change
    rows.append((m, now, ref, change, delta))

deltaOf = {r[0]: r[4] for r in rows if r[4] is not None}

def confirmed(m):
    sc = scenarioOf(m)
    return any(deltaOf.get(k) is not None and deltaOf[k] <= -WARN_PCT for k in (sc, sc + "_p50us"))

worst, verdict = 0.0, 0
for m, now, ref, change, delta in rows:
    if ref is None:
        print(f"| {m} | {now} | — | first run |")
        continue
    counts = not (isTail(m) and delta <= -FAIL_PCT and not confirmed(m))
    if delta >= -WARN_PCT:
        mark = ""
    elif delta > -FAIL_PCT or not counts:
        mark = " ⚠️"
    else:
        mark = " ❌"
    note = "latency, lower is better" if lowerIsBetter(m) else ""
    if not counts:
        note = "tail only - throughput and p50 of this scenario did not move"
    print(f"| {m} | {now} | {ref:.0f} | {change:+.1f}%{mark} | {note} |")
    if counts:
        worst = min(worst, delta)
        if delta <= -FAIL_PCT:
            verdict = 1
    else:
        worst = min(worst, -WARN_PCT)

print()
if worst <= -FAIL_PCT:
    print(f"**A metric collapsed by {abs(worst):.0f}%** against the median of the last {WINDOW} runs. "
          "That is past what runner noise explains.")
elif worst <= -WARN_PCT:
    print(f"Slower by {abs(worst):.0f}% against the median of the last {WINDOW} runs — worth a look, "
          "though a shared runner can move this much on its own.")
else:
    print(f"Within noise ({worst:+.1f}% worst case).")

sys.exit(verdict)
