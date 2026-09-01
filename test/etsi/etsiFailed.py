#!/usr/bin/env python3
#
# etsiFailed.py - the names of the tests that FAILED in a robot output.xml.
#
# Two callers, one job: deciding whether a rerun is warranted at all, and
# recording which tests failed on the first pass so the summary can name the
# ones that then healed. A flake is reported, never hidden.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()

for test in root.iter("test"):
    status = test.find("status")
    if status is not None and status.get("status") == "FAIL":
        print(test.get("name"))
