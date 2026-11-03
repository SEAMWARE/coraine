#!/usr/bin/env python3
#
# etsiFailed.py - the names of the tests that FAILED in a robot output.xml.
#
# Two callers, one job: deciding whether a rerun is warranted at all, and
# recording which tests failed on the first pass so the summary can name the
# ones that then healed. A flake is reported, never hidden.
#
# A test purpose listed in deviations.txt (next to this script) fails on purpose
# and is left out - etsiSummary.py names it.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import os
import sys
import xml.etree.ElementTree as ET


def deviations():
    """The ids in deviations.txt: test purposes coraine fails on purpose."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "deviations.txt")
    ids = {}
    try:
        with open(path) as fp:
            for line in fp:
                line = line.strip()
                if line and not line.startswith("#"):
                    tpId, _, why = line.partition(" ")
                    ids[tpId] = why.strip()
    except OSError:
        pass
    return ids


def tpId(name):
    """A robot test name starts with its test purpose id: '013_04 Delete Scope ...'."""
    return (name or "").split(" ")[0]


if __name__ == "__main__":
    root      = ET.parse(sys.argv[1]).getroot()
    deviating = deviations()

    for test in root.iter("test"):
        status = test.find("status")
        if status is not None and status.get("status") == "FAIL" and tpId(test.get("name")) not in deviating:
            print(test.get("name"))
