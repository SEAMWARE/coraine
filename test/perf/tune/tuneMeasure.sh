#!/bin/bash
#
# tuneMeasure.sh <workload.json> <coraine> <build-plugins-dir> - the knobs measured on the workload
#
# --dbLockPrefer reads|writes x the allocator (glibc's; jemalloc and tcmalloc where installed), each run
# TUNE_REPEATS (2) times, the combinations alternated so a drift of the machine spreads over all of them.
# The broker as built - `make tune` hands it the profile-guided one. Prints tune-result.json: the table,
# the winner, and how to run it.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u
W=$(realpath "$1"); B=$2; SRC=$3
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

#
# A comparison on a busy machine compares the machine: say so if a browser is up (the usual culprit)
#
for app in chrome chromium firefox; do
  pgrep -x "$app" > /dev/null && echo "tuneMeasure: WARNING - $app is running: the numbers measure it too. Close it, or take them with care." >&2
done

python3 "$HERE/tuneGen.py" "$W" "$TMP/gen" >&2 || exit 1
"$HERE/tunePlugins.sh" "$SRC" "$TMP/plugins" || exit 1

read -r conc dur < <(python3 -c "import json,sys; w=json.load(open(sys.argv[1])); print(w.get('load',{}).get('concurrency',50), w.get('duration','30s'))" "$W")

allocs=("glibc=")
for lib in libjemalloc.so.2 libtcmalloc_minimal.so.4; do
  p=$(ldconfig -p 2>/dev/null | awk -v l="$lib" '$1 == l { print $NF; exit }')
  [ -n "$p" ] && allocs+=("${lib%%.so*}=$p")
done

for rep in $(seq 1 "${TUNE_REPEATS:-2}"); do
  for prefer in reads writes; do
    for a in "${allocs[@]}"; do
      name=${a%%=*}; pre=${a#*=}
      echo "tuneMeasure: run $rep, --dbLockPrefer $prefer, allocator ${name#lib}" >&2
      line=$(TUNE_PRELOAD=$pre TUNE_DURATION=$dur TUNE_CONCURRENCY=$conc "$HERE/tuneRun.sh" "$TMP/gen" "$B" "$TMP/plugins" --dbLockPrefer "$prefer")
      [ -n "$line" ] && echo "$prefer ${name#lib} $pre $line" >> "$TMP/runs"
    done
  done
done

python3 - "$TMP/runs" "$W" <<'PYEOF'
import json, sys, statistics as st
runs = {}
for l in open(sys.argv[1]):
    prefer, alloc, pre, js = l.split(" ", 3)
    runs.setdefault((prefer, alloc, pre), []).append(json.loads(js))
table = []
for (prefer, alloc, pre), rs in runs.items():
    table.append({"dbLockPrefer": prefer, "allocator": alloc, "preload": pre or None,
                  "rps": round(st.mean(r["rps"] for r in rs)), "p99us": round(st.mean(r["p99us"] for r in rs)),
                  "notifiedPerRun": round(st.mean(r["notified"] for r in rs)), "failedRequests": sum(r.get("non2xx", 0) for r in rs),
                  "runs": [r["rps"] for r in rs]})
table.sort(key=lambda r: -r["rps"])
base = next(r for r in table if r["dbLockPrefer"] == "reads" and r["allocator"] == "glibc")
for r in table:
    r["vsDefault"] = "%+.1f %%" % ((r["rps"] - base["rps"]) / base["rps"] * 100)
# A combination that is fast because requests FAIL does not win
clean = [r for r in table if r["failedRequests"] == 0]
if not clean:
    sys.exit("tuneMeasure: every combination had failed requests - the workload does not run as described (tune-result: none)")
best = clean[0]
print(json.dumps({"workload": sys.argv[2], "best": {
    "dbLockPrefer": best["dbLockPrefer"], "allocator": best["allocator"],
    "brokerOptions": "--dbLockPrefer " + best["dbLockPrefer"],
    "env": ({"LD_PRELOAD": best["preload"]} if best["preload"] else {}),
    "rps": best["rps"], "vsDefault": best["vsDefault"]}, "table": table}, indent=2))
PYEOF
