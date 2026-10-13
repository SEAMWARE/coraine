#!/usr/bin/env bash
#
# every-switch.sh [<dir>] - a release build of coraine for each feature switch turned from its default:
# each default-on switch off, each default-off one on - one build tree per switch, in <dir> (default
# ./BUILD_SWITCHES). The cor-libs are built already (`make release` once, or corLibs' bootstrap.sh).
#
# A switch is tested by the functests only in the position a test asks for (REQUIRE_FEATURE /
# SKIP_FEATURE) and only on a build that has it - most never are, so this is what says each one still
# builds and links: a route table naming a handler its switch compiled out builds, and fails to link.
# The list comes from `coraine-build --list-features --json` (CMakeLists.txt), so a new switch is in it.
#
# Exit 1 when any build fails; each one's output in <dir>/<switch>.log.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u

TOP=$(cd "$(dirname "$0")/../.." && pwd)
DIR=$(mkdir -p "${1:-$TOP/BUILD_SWITCHES}" && cd "${1:-$TOP/BUILD_SWITCHES}" && pwd)
JOBS=$(nproc)

#
# The switches and their defaults, from CMakeLists.txt's option() lines - what --list-features reads
#
mapfile -t switches < <(sed -n 's/^option(COR_FEATURE_\([A-Z_0-9]*\) .* \(ON\|OFF\))$/\1 \2/p' "$TOP/CMakeLists.txt")

if [ ${#switches[@]} -eq 0 ]; then
  echo "every-switch.sh: no COR_FEATURE_* option in $TOP/CMakeLists.txt" >&2
  exit 1
fi

failed=()
for s in "${switches[@]}"; do
  name=${s% *}
  default=${s#* }
  value=$([ "$default" = ON ] && echo OFF || echo ON)
  icu=OFF
  [ "$name" = ICU_COLLATION ] && icu=$value

  if cmake -S "$TOP" -B "$DIR/$name" -DCMAKE_BUILD_TYPE=Release -DCOR_FEATURE_ICU_COLLATION=$icu -DCOR_FEATURE_$name=$value > "$DIR/$name.log" 2>&1 \
     && cmake --build "$DIR/$name" -j"$JOBS" >> "$DIR/$name.log" 2>&1 \
     && "$DIR/$name/src/app/coraine/coraine" --version | grep -qw "$name=$([ $value = ON ] && echo 1 || echo 0)"; then
    echo "  $name=$value: builds"
  else
    echo "  $name=$value: FAILED - $DIR/$name.log"
    grep -m5 -E "error|undefined reference" "$DIR/$name.log" | sed 's/^/      /'
    failed+=("$name=$value")
  fi
done

if [ ${#failed[@]} -gt 0 ]; then
  echo "every-switch.sh: ${#failed[@]} of ${#switches[@]} do not build: ${failed[*]}"
  exit 1
fi

echo "every-switch.sh: all ${#switches[@]} switches build, each turned from its default"
