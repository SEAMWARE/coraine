#!/usr/bin/env python3
#
# etsiSummary.py - the ETSI run in a form a person can read.
#
# The console is quiet now (ETSI_QUIET_IO), so the run summary is where the result
# lives: the counts, and every failure with its first line of explanation. The full
# detail stays in log.html and output.xml, which are uploaded as artifacts.
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

print(f"### ETSI conformance — {passed}/{total} ({rate:.1f}%)\n")

if not failed:
    print("Every test purpose passed.")
    sys.exit(0)

print(f"**{len(failed)} failed:**\n")
print("| test | reason |")
print("|---|---|")
for name, why in failed:
    print(f"| `{name}` | {why.replace('|', '\\|')} |")
