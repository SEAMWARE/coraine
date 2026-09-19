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
import json, sys, statistics

WARN_PCT, FAIL_PCT, WINDOW = 20.0, 50.0, 5


#
# ⭐ Not every metric is "bigger is better".
#
# The throughput metrics are requests/second - up is good. The `_p99us` ones
# are p99 LATENCY in microseconds - down is good. Comparing them the same way
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
def lowerIsBetter(metric):
    return metric.endswith("_p99us")

historyFile, todayJson = sys.argv[1], sys.argv[2]
today = json.loads(todayJson)
db    = today["db"]

history = []
try:
    for line in open(historyFile):
        line = line.strip()
        if line:
            rec = json.loads(line)
            if rec.get("db") == db:
                history.append(rec)
except FileNotFoundError:
    pass

metrics = [k for k in today if k not in ("db", "date", "sha", "run")]
print(f"### Performance — `{db}`\n")
print("| metric | now | median of last %d | change | | " % WINDOW)
print("|---|---:|---:|---:|---|")

worst, verdict = 0.0, 0
for m in metrics:
    now  = today[m]
    past = [r[m] for r in history[-WINDOW:] if m in r]
    if not past:
        print(f"| {m} | {now} | — | first run |")
        continue
    ref    = statistics.median(past)
    change = (now - ref) / ref * 100.0
    # `delta` is the change expressed as "better or worse", whichever way the
    # metric runs. Every threshold below is on delta; the table still prints
    # the true change, so the numbers can be checked against the raw records.
    delta  = -change if lowerIsBetter(m) else change
    mark   = "" if delta >= -WARN_PCT else (" ⚠️" if delta > -FAIL_PCT else " ❌")
    note   = "latency, lower is better" if lowerIsBetter(m) else ""
    print(f"| {m} | {now} | {ref:.0f} | {change:+.1f}%{mark} | {note} |")
    worst = min(worst, delta)
    if delta <= -FAIL_PCT:
        verdict = 1

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
