#!/usr/bin/env python3
#
# perfRecord.py - turn one measurement into the line that goes into the history.
#
# A file rather than a heredoc inside the workflow: an indented heredoc body with
# a quoted delimiter hands python leading spaces and an IndentationError, and a
# workflow is a bad place to discover that. It is also testable from a shell.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import json, os, sys

#
# cpu - the machine, as "<model name> x<logical CPUs>". A shared runner is not one machine: the
# same commit measured on two CPU generations differs by 2x on every metric at once. perfCompare
# compares a run only with runs on the same CPU.
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

db  = sys.argv[1]
rec = json.load(open(f"/tmp/perf-{db}.json"))

rec["sha"]  = os.environ.get("GITHUB_SHA", "local")[:8]
rec["run"]  = os.environ.get("GITHUB_RUN_ID", "-")
rec["date"] = os.environ.get("RUN_DATE", "")
rec["cpu"]  = cpu()

print(json.dumps(rec, sort_keys=True))
