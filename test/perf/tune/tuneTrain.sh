#!/bin/bash
#
# tuneTrain.sh <coraine> <build-plugins-dir> - the training run of `make tune`: the profile-guided build
# trained on the WORKLOAD (TUNE_WORKLOAD, a workload file - doc/extreme-performance.md), not on
# pgoTrain.sh's general shapes
#
# The workload runs twice - once with each --dbLockPrefer, so the code of whichever wins is in the
# profile. Short (TUNE_TRAIN_DURATION, 20s each): a profile needs the shape of the work, not its length.
# The broker is stopped with SIGTERM by tuneRun.sh - a clean exit is what writes the profile.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u
B=$1; SRC=$2
HERE=$(cd "$(dirname "$0")" && pwd)
[ -n "${TUNE_WORKLOAD:-}" ] || { echo "tuneTrain: TUNE_WORKLOAD - the workload file" >&2; exit 1; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

python3 "$HERE/tuneGen.py" "$TUNE_WORKLOAD" "$TMP/gen" || exit 1
"$HERE/tunePlugins.sh" "$SRC" "$TMP/plugins" || exit 1

conc=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('load',{}).get('concurrency',50))" "$TUNE_WORKLOAD")
for prefer in reads writes; do
  echo "tuneTrain: the workload, --dbLockPrefer $prefer"
  TUNE_WARMUP=2s TUNE_DURATION=${TUNE_TRAIN_DURATION:-20s} TUNE_CONCURRENCY=$conc \
    "$HERE/tuneRun.sh" "$TMP/gen" "$B" "$TMP/plugins" --dbLockPrefer "$prefer" || echo "tuneTrain: the run with $prefer failed - the profile lacks it"
done
