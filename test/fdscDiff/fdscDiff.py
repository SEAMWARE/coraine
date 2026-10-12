#!/usr/bin/env python3
#
# FILE            fdscDiff.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# The differential half of fdscDiff.sh: every case of cases.json sent through the Data Space Connector's
# chain (APISIX -> plain broker) and to coraine with --auth fdsc, and the two answers compared by class:
# 401, 403, 503, or "in" (let through - whatever the broker then said). The standard library only.
#
#   fdscDiff.py --apisix URL --coraine URL --cases FILE --keys DIR --iss ISS [--known FILE]
#
# --known: a JSON object {case name: reason} - differences that are decided, printed and not failed on.
# Exit 0: no difference outside --known.
#
import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

FTJWT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "funcTests", "tools", "ftJwt.py")


def token(spec, keys, iss):
    if spec is None:
        return None
    key = spec.get("key", "k1")
    alg = "ES256" if key == "k2" else "RS256"
    args = [sys.executable, FTJWT, "sign", "--key", os.path.join(keys, key + ".pem"), "--alg", alg,
            "--iss", spec.get("iss", iss), "--exp", str(spec.get("exp", 1800))]
    if "kid" in spec:
        args += ["--kid", spec["kid"]]
    if "vc" in spec:
        args += ["--vc", json.dumps(spec["vc"])]
    if "vp" in spec:
        args += ["--vp", json.dumps(spec["vp"])]
    if spec.get("badSig"):
        args.append("--badSig")
    return subprocess.run(args, capture_output=True, check=True, text=True).stdout.strip()


def send(base, case, tok, spec):
    headers = dict(case.get("headers", {}))
    if tok is not None:
        if spec.get("header") == "X-Access-Token":
            headers["X-Access-Token"] = tok
        else:
            headers["Authorization"] = spec.get("scheme", "Bearer") + " " + tok
    data = None
    if case.get("body") is not None:
        data = json.dumps(case["body"]).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + case["path"], data=data, method=case["method"], headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except Exception as e:
        return "error: %s" % e


def klass(status):
    return str(status) if status in (401, 403, 503) else "in"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--apisix", required=True)
    p.add_argument("--coraine", required=True)
    p.add_argument("--cases", required=True)
    p.add_argument("--keys", required=True)
    p.add_argument("--iss", required=True)
    p.add_argument("--known")
    a = p.parse_args()

    cases = json.load(open(a.cases))
    known = json.load(open(a.known)) if a.known else {}
    diffs = 0
    width = max(len(c["name"]) for c in cases)

    print("%-*s  %-6s %-6s" % (width, "case", "FDSC", "coraine"))
    for c in cases:
        spec = c.get("token")
        tok = token(spec, a.keys, a.iss)
        f = send(a.apisix, c, tok, spec or {})
        k = send(a.coraine, c, tok, spec or {})
        same = klass(f) == klass(k)
        mark = "" if same else ("  KNOWN: " + known[c["name"]] if c["name"] in known else "  DIFF")
        if not same and c["name"] not in known:
            diffs += 1
        print("%-*s  %-6s %-6s%s" % (width, c["name"], f, k, mark))

    print()
    print("%d cases, %d differences%s" % (len(cases), diffs, "" if diffs == 0 else " - see DIFF"))
    return 0 if diffs == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
