#!/usr/bin/env bash
#
# source.sh <out-dir> - the complete source of one coraine build, as ONE tarball
#
#   <out-dir>/coraine-src.tar.xz   coraine-<version>/
#                                    SOURCE                  version, gitSha, commit date (key=value)
#                                    MANIFEST.txt            every repo and the commit that went in
#                                    coraine/                git archive HEAD of this checkout
#                                    corBase/ ... corLibs/   each Cor-Lib, exported by docker/vendor-libs.sh
#   <out-dir>/version              the upstream version (version.sh)
#
# It is what the packages are built from (build.sh) and what coraine-dev ships in /usr/share/coraine/src,
# so `coraine-build` rebuilds exactly the source the installed broker came from.
#
# Committed trees only: coraine is `git archive HEAD`, each lib `git archive $REF` (vendor-libs.sh) - an
# uncommitted change is in neither, and MANIFEST.txt says so for a lib.
#
#   BASE  where the Cor-Lib clones are (default: the parent of this checkout - siblings)
#   REF   what of each lib (default HEAD: the branch each clone has checked out; origin/main ...)
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

OUT=$(mkdir -p "${1:?usage: source.sh <out-dir>}" && cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
BASE=${BASE:-$(dirname "$TOP")}
REF=${REF:-HEAD}

VERSION=$("$HERE/version.sh")
SHA=$(git -C "$TOP" rev-parse HEAD)
DATE=$(git -C "$TOP" log -1 --format=%cI)

if [ -n "$(git -C "$TOP" status --porcelain --untracked-files=no)" ]; then
  echo "source.sh: coraine has uncommitted changes - they are NOT in the tarball" >&2
fi

# the Cor-Libs, into docker/vendor/ (the image build uses the same export)
BASE=$BASE REF=$REF "$TOP/docker/vendor-libs.sh" > /dev/null

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT=$WORK/coraine-$VERSION
mkdir -p "$ROOT"

git -C "$TOP" archive --format=tar --prefix=coraine/ HEAD | tar -xf - -C "$ROOT"
for t in "$TOP"/docker/vendor/*.tar; do
  tar -xf "$t" -C "$ROOT"
done

#
# MANIFEST.txt: vendor-libs.sh's, plus coraine itself. A copy goes where stackManifest.sh looks for it
# (coraine/docker/vendor/MANIFEST.txt) - a tree without .git then still reports each library's commit
# on GET /version.
#
{
  cat "$TOP/docker/vendor/MANIFEST.txt"
  printf '%-10s %-24s %s\n' coraine "$(git -C "$TOP" rev-parse --abbrev-ref HEAD)" "$SHA"
} > "$ROOT/MANIFEST.txt"
mkdir -p "$ROOT/coraine/docker/vendor"
cp "$ROOT/MANIFEST.txt" "$ROOT/coraine/docker/vendor/MANIFEST.txt"

printf 'version=%s\ngitSha=%s\ncommitDate=%s\n' "$VERSION" "$SHA" "$DATE" > "$ROOT/SOURCE"

# reproducible: fixed owner, order and mtime (the commit's)
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="$DATE" -C "$WORK" -cf - "coraine-$VERSION" \
  | xz -T0 -9 > "$OUT/coraine-src.tar.xz"
echo "$VERSION" > "$OUT/version"

echo "source.sh: $OUT/coraine-src.tar.xz ($(du -h "$OUT/coraine-src.tar.xz" | cut -f1)) - coraine $VERSION"
cat "$ROOT/MANIFEST.txt"
