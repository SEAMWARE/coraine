#!/usr/bin/env python3
#
# tuneEnv.py <tune-result.json> - the best of a `make tune` run as a shell file for the image's entry point
# (docker/coraine-start): TUNE_DBLOCKPREFER, TUNE_ALLOCATOR
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import json, sys

r = json.load(open(sys.argv[1]))
b = r["best"]
print("# make tune - %s: %s requests/s, %s against the out-of-the-box settings" % (r["workload"].split("/")[-1], b["rps"], b["vsDefault"]))
print("TUNE_DBLOCKPREFER=%s" % b["dbLockPrefer"])
print("TUNE_ALLOCATOR=%s" % b["allocator"])
