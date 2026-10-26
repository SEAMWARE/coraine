#!/bin/bash
#
# FILE            optionsDocumented.sh
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# optionsDocumented.sh - every command-line option documented in doc/options.md, and nothing documented that is gone
#
# Usage: optionsDocumented.sh <doc/options.md> <coraine> [<coraine-import>] [<coraine-build>]
#
# The options are what the programs SAY they take: `coraine -u` with each plugin that adds options loaded (a plugin's
# options are listed only when it is), `coraine-import -u`, `coraine-build --help`. Each must have its entry -
# a heading "### `--name`" - in its part of the document: the broker's anywhere before "## coraine-import",
# coraine-import's in that section, coraine-build's in "## coraine-build".
#
# The other way: an entry no program lists fails too, unless its first paragraph says "build:" - an
# option that only some builds have (a feature switch, the HTTP server), which this build may lack - or
# "deprecated:" - an option still accepted and no longer listed (-fg).
#
# Prints one line per problem and exits 1; prints "all N options documented" and exits 0.
#
doc="${1:?usage: optionsDocumented.sh <doc/options.md> <coraine> [<coraine-import>] [<coraine-build>]}"
broker="${2:?usage: optionsDocumented.sh <doc/options.md> <coraine> [<coraine-import>] [<coraine-build>]}"
import="${3:-}"
build="${4:-}"

[ -r "$doc" ] || { echo "optionsDocumented.sh: cannot read $doc"; exit 1; }

problems=0
problem() { echo "$1"; problems=$((problems + 1)); }

#
# listed <command...> - the long options a usage text names: '[--name' and, for coraine-build's help, a line
# that starts with the option: '  --name' or '  -h, --name'
#
listed() {
  "$@" 2>&1 | grep -o -E '(\[|^ +(-[A-Za-z], )?)--[A-Za-z][A-Za-z0-9-]*' | grep -o -E -- '--[A-Za-z][A-Za-z0-9-]*$' | sort -u
}

#
# documented <from> <to> - the entries between two h2 headings (by their text; empty: the start / the end)
#
documented() {
  awk -v from="$1" -v to="$2" '
    BEGIN               { on = (from == "") }
    /^## /              { if (from != "" && index($0, "## " from) == 1) on = 1; else if (to != "" && index($0, "## " to) == 1) on = 0 }
    on && /^### `--/    { sub(/^### `/, ""); sub(/`.*/, ""); print }
  ' "$doc" | sort -u
}

#
# buildDependent <name> - the entry's first paragraph (the lines under its heading, up to the first empty one)
# says "build:" or "deprecated:". The verdict is set in END: an `exit` in a rule runs END, whose own exit
# code is the one that counts.
#
buildDependent() {
  awk -v h="### \`$1\`" '
    $0 == h                                  { on = 1; next }
    on == 1 && NF                            { on = 2 }
    on == 2 && !NF                           { exit }
    on == 2 && /build:|deprecated:/          { found = 1; exit }
    END                                      { exit (found ? 0 : 1) }
  ' "$doc"
}

#
# The broker: plain, then each plugin that adds options. A plugin missing from this installation is a problem -
# its options could not be checked
#
brokerListed=""
for args in "-db mongoc" "-db corDB" "-db corDB --troe timescale" "-db corDB --auth fdsc" "-db corDB --auth oidc"; do
  out=$(env -u SEAMWARE_PLUGIN_DIR "$broker" -u $args 2>&1)
  echo "$out" | grep -q -E "^[a-zA-Z]+ plugin '[^']*' .*: not found" && problem "coraine $args: a plugin is not found - its options are not checked"
  brokerListed="$brokerListed"$'\n'"$(echo "$out" | grep -o -E '\[--[A-Za-z][A-Za-z0-9-]*' | sed 's/^\[//')"
done
brokerListed=$(echo "$brokerListed" | grep . | sort -u)
brokerDoc=$(documented "" "coraine-import")

for o in $brokerListed; do
  echo "$brokerDoc" | grep -q -x -- "$o" || problem "coraine: $o is not documented"
done
for o in $brokerDoc; do
  echo "$brokerListed" | grep -q -x -- "$o" && continue
  buildDependent "$o" || problem "coraine: $o is documented, and no listing has it (gone? or say 'build:' / 'deprecated:')"
done
total=$(echo "$brokerListed" | grep -c .)

if [ -n "$import" ]; then
  l=$(listed "$import" -u)
  d=$(documented "coraine-import" "coraine-build")
  for o in $l; do echo "$d" | grep -q -x -- "$o" || problem "coraine-import: $o is not documented"; done
  for o in $d; do echo "$l" | grep -q -x -- "$o" || problem "coraine-import: $o is documented, and coraine-import -u does not list it"; done
  total=$((total + $(echo "$l" | grep -c .)))
fi

if [ -n "$build" ]; then
  l=$(listed "$build" --help)
  d=$(documented "coraine-build" "")
  for o in $l; do echo "$d" | grep -q -x -- "$o" || problem "coraine-build: $o is not documented"; done
  for o in $d; do echo "$l" | grep -q -x -- "$o" || problem "coraine-build: $o is documented, and coraine-build --help does not list it"; done
  total=$((total + $(echo "$l" | grep -c .)))
fi

if [ "$problems" -ne 0 ]; then
  echo "$problems problem(s) - doc/options.md documents every option"
  exit 1
fi
echo "all $total options documented"
