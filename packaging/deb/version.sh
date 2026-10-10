#!/usr/bin/env bash
#
# version.sh - the upstream version of the Debian packages built from this checkout
#
#   HEAD at the release tag v<X>   ->  <X>                         e.g. 0.4.0
#   any other commit               ->  <X>~git<YYYYMMDD>.<sha8>    e.g. 0.4.0~git20261008.04785a2c
#
# <X> is CORAINE_VERSION in src/app/coraine/coraineVersion.h - what the broker reports on /version -
# without a pre-release suffix: main's "0.6.0-dev" gives 0.6.0~git... The
# date is the COMMIT's (UTC), not the build's: two builds of one commit get one version. '~' sorts
# before everything, so a git build sorts before the release of the same <X> - and after every git
# build of an earlier day. Two merges on the same day sort by sha, i.e. arbitrarily.
#
# The distribution suffix (+ubuntu26.04 ...) is not added here: build.sh adds it, inside the container
# that knows which distribution it is.
#
# DEB_UPSTREAM_VERSION in the environment wins.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

if [ -n "${DEB_UPSTREAM_VERSION:-}" ]; then
  echo "$DEB_UPSTREAM_VERSION"
  exit 0
fi

TOP=$(cd "$(dirname "$0")/../.." && pwd)
v=$(sed -n 's/^#define CORAINE_VERSION[[:space:]]*"\(.*\)"/\1/p' "$TOP/src/app/coraine/coraineVersion.h")
[ -n "$v" ] || { echo "version.sh: no CORAINE_VERSION in coraineVersion.h" >&2; exit 1; }
v=${v%%-*}       # "0.6.0-dev" on main -> "0.6.0": the release it leads to, a git build sorting before it

if git -C "$TOP" describe --exact-match --tags --match "v$v" HEAD >/dev/null 2>&1; then
  echo "$v"
else
  echo "$v~git$(TZ=UTC git -C "$TOP" log -1 --format=%cd --date=format-local:%Y%m%d).$(git -C "$TOP" rev-parse --short=8 HEAD)"
fi
