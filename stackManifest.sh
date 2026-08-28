#!/bin/bash
#
# stackManifest.sh - emit the resolved commit of every library the broker links.
#
# The broker is largely library code by volume: seven k-libs and four Cor-Libs.
# The cor* repos track `main` by design (see corLibs/bootstrap.sh), so a published
# image records coraine's own sha and nothing about the rest of what is in it -
# "which coraine is this" is only half an answer.
#
# The k-libs LOOK pinned and are not: klib-pins names release BRANCHES, and a
# branch moves. That is the layer this matters most for, because it is the one
# that gives a false sense of reproducibility.
#
# Resolution order per library, so this works everywhere the broker is built:
#   1. a git checkout beside us    - the developer's tree, and the k-libs inside
#                                    the image (cloned there, shallow but real)
#   2. docker/vendor/MANIFEST.txt  - the Cor-Libs in the image, which arrive as
#                                    tarballs and have no .git at all
#   3. "unknown"                   - honest, rather than a guess
#
# Output: one `<name> <sha>` per line, in link order.
#
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SIBLINGS=${SIBLING_DIR:-$(dirname "$HERE")}
MANIFEST="$HERE/docker/vendor/MANIFEST.txt"

LIBS="kbase kalloc klog khash kjson kargs ktrace kprom corRest corNgsild corJsonld corPlugin"

for lib in $LIBS; do
  sha=""

  if [ -d "$SIBLINGS/$lib/.git" ]; then
    sha=$(git -C "$SIBLINGS/$lib" rev-parse HEAD 2>/dev/null)
  fi

  if [ -z "$sha" ] && [ -f "$MANIFEST" ]; then
    sha=$(awk -v l="$lib" '$1 == l { print $3 }' "$MANIFEST" | head -1)
  fi

  printf '%s %s\n' "$lib" "${sha:-unknown}"
done
