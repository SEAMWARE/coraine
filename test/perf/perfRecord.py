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

db  = sys.argv[1]
rec = json.load(open(f"/tmp/perf-{db}.json"))

rec["sha"]  = os.environ.get("GITHUB_SHA", "local")[:8]
rec["run"]  = os.environ.get("GITHUB_RUN_ID", "-")
rec["date"] = os.environ.get("RUN_DATE", "")

print(json.dumps(rec, sort_keys=True))
