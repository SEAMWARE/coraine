#!/usr/bin/env python3
#
# etsiSummary.py - the ETSI run in a form a person can read.
#
# The console is quiet now (ETSI_QUIET_IO), so the run summary is where the result
# lives: the counts, and every failure with its first line of explanation. The full
# detail stays in log.html and output.xml, which are uploaded as artifacts.
#
# Usage: etsiSummary.py <output.xml> [first-pass-failures.txt]
#
# The optional second argument is the list of tests that failed BEFORE the rerun.
# Anything in it that passes in the merged result healed on the second attempt -
# a flake - and is named here. Healed, never hidden: a run that reports 1046/1046
# while quietly retrying the same test every night is worse than one that fails.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()

passed, failed = 0, []
for test in root.iter("test"):
    status = test.find("status")
    if status is None:
        continue
    if status.get("status") == "PASS":
        passed += 1
    else:
        first = (status.text or "").strip().split("\n")[0]
        failed.append((test.get("name"), first[:120]))

total = passed + len(failed)
rate  = (passed / total * 100.0) if total else 0.0

#
# Tests that failed on the first pass and pass in the merged result: the rerun
# healed them, so they are flaky rather than broken. Name them.
#
flaky = []
if len(sys.argv) > 2:
    try:
        with open(sys.argv[2]) as fp:
            firstPassFailures = {line.strip() for line in fp if line.strip()}
    except OSError:
        firstPassFailures = set()

    stillFailing = {name for name, _ in failed}
    flaky = sorted(firstPassFailures - stillFailing)

print(f"### ETSI conformance — {passed}/{total} ({rate:.1f}%)\n")

if flaky:
    print(f"**{len(flaky)} healed on a rerun** (flaky, not broken):\n")
    for name in flaky:
        print(f"- `{name}`")
    print()

if not failed:
    print("Every test purpose passed.")
    sys.exit(0)

print(f"**{len(failed)} failed:**\n")
print("| test | reason |")
print("|---|---|")
for name, why in failed:
    print(f"| `{name}` | {why.replace('|', '\\|')} |")
