#!/bin/bash
#
# tunePlugins.sh <build-plugins-dir> <outDir> - a build's plugins, laid out as the broker loads them
#
# The build's own (BUILD_PGO/src/plugins: mongoc, the temporal plugins) and corDB's from its own repo -
# built by the same `make pgo`/`make tune`, in obj/release. Point SEAMWARE_PLUGIN_DIR at <outDir>.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -eu
SRC=$(realpath "$1"); OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)
CORDB=${COR_DB_DIR:-$(cd "$HERE/../../../../corDB" && pwd)}

mkdir -p "$OUT/db/currentState" "$OUT/troe/temporal" "$OUT/api" "$OUT/bridge"
cp "$CORDB/obj/release/corDB.so"                     "$OUT/db/currentState/"
[ -f "$SRC/currentState/mongoc/mongoc.so" ] && cp "$SRC/currentState/mongoc/mongoc.so" "$OUT/db/currentState/"
cp "$SRC/temporal/none/none.so"                      "$OUT/troe/temporal/"
[ -f "$SRC/temporal/timescale/timescale.so" ] && cp "$SRC/temporal/timescale/timescale.so" "$OUT/troe/temporal/"
exit 0
