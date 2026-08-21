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
print("| metric | now | median of last %d | change |" % WINDOW)
print("|---|---:|---:|---:|")

worst, verdict = 0.0, 0
for m in metrics:
    now  = today[m]
    past = [r[m] for r in history[-WINDOW:] if m in r]
    if not past:
        print(f"| {m} | {now} | — | first run |")
        continue
    ref    = statistics.median(past)
    change = (now - ref) / ref * 100.0
    mark   = "" if change >= -WARN_PCT else (" ⚠️" if change > -FAIL_PCT else " ❌")
    print(f"| {m} | {now} | {ref:.0f} | {change:+.1f}%{mark} |")
    worst = min(worst, change)
    if change <= -FAIL_PCT:
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
