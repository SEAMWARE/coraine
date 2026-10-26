#!/bin/bash
#
# pluginOptions.sh - each plugin's options, documented in its own doc as in doc/options.md.
#
# doc/options.md is the list of every option, a section per plugin ("## Plugin: <name> - ..."). A plugin's own
# doc has its options too - the same entries, copied by this script between two marker lines:
#
#   <!-- options of the <name> plugin: copied from doc/options.md by tools/pluginOptions.sh - edit them there -->
#   <!-- end of the options of the <name> plugin -->
#
# An entry is changed in doc/options.md only; this script then rewrites the copies. Copied, the headings are
# moved down to fit where the copy sits, and the relative links are made to work from there: 'x.md' and
# '#anchor' (an anchor of options.md) become paths to doc/ - or, for a doc in another repository, the URL of
# coraine's doc on GitHub.
#
# The copies (plugin, file, heading levels down, the link base):
#   mongoc     src/plugins/currentState/mongoc/README.md
#   timescale  src/plugins/temporal/timescale/README.md
#   oidc       doc/authorization.md (§ 8.1)
#   fdsc       doc/authorization.md (§ 3.1)
#   corDB      ../corDB/README.md - the corDB repository beside this one; written when it is there, never
#              checked (a check here would turn main red between the two repositories' merges)
#
# Usage:
#   pluginOptions.sh            rewrites the copies
#   pluginOptions.sh --check    every copy in this repository as doc/options.md has it - exit 1, naming the
#                               ones that are not (CI)
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u
set -o pipefail

top=$(git rev-parse --show-toplevel) || exit 1
cd "$top" || exit 1

GITHUB_DOC="https://github.com/SEAMWARE/coraine/blob/main/doc/"

#   plugin     file                                           levels  link base             checked
COPIES="
mongoc     src/plugins/currentState/mongoc/README.md       0       ../../../../doc/      yes
timescale  src/plugins/temporal/timescale/README.md        0       ../../../../doc/      yes
oidc       doc/authorization.md                            1       -                     yes
fdsc       doc/authorization.md                            1       -                     yes
corDB      ../corDB/README.md                              0       $GITHUB_DOC           no
"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

#
# entries <plugin> <levels> <base> - the plugin's entries in doc/options.md, as they are copied: from its first
# '### `--' heading to the end of its section, headings <levels> down, links made relative to <base> ('-': doc/)
#
entries() {
  awk -v p="$1" -v lv="$2" -v base="$3" '
    function rel(s,   out, pre, t) {
      # ](#a) -> ](<base>options.md#a); ](x.md...) -> ](<base>x.md...); http(s) and absolute stay
      out = ""
      while (match(s, /\]\([^)]*\)/)) {
        pre = substr(s, 1, RSTART - 1)
        t = substr(s, RSTART + 2, RLENGTH - 3)
        if (t ~ /^#/)                                 t = (base == "-" ? "options.md" : base "options.md") t
        else if (t !~ /^[a-z]+:/ && base != "-")      t = base t
        out = out pre "](" t ")"
        s = substr(s, RSTART + RLENGTH)
      }
      return out s
    }
    index($0, "## Plugin: " p " ") == 1      { on = 1; next }
    on && /^## /                              { exit }
    on && !started && /^### `--/              { started = 1 }
    !started                                  { next }
    /^---$/                                   { next }
    /^#+ /                                    { h = $0; for (i = 0; i < lv; i++) h = "#" h; print rel(h); next }
                                              { print rel($0) }
  ' doc/options.md | awk '{ l[++n] = $0 } END { while (n > 0 && l[n] ~ /^[[:space:]]*$/) n--; for (i = 1; i <= n; i++) print l[i] }'
}

#
# copied <plugin> <file> - the lines between the plugin's markers in <file> (exit 2: no markers)
#
copied() {
  awk -v b="<!-- options of the $1 plugin: " -v e="<!-- end of the options of the $1 plugin -->" '
    index($0, b) == 1   { on = 1; found = 1; next }
    $0 == e             { on = 0; next }
    on                  { print }
    END                 { if (!found) exit 2 }
  ' "$2"
}

#
# write <plugin> <file> <entries file> - the copy in <file> replaced by <entries file>, an empty line around it
#
write() {
  awk -v b="<!-- options of the $1 plugin: " -v e="<!-- end of the options of the $1 plugin -->" -v f="$3" '
    index($0, b) == 1   { print; print ""; while ((getline l < f) > 0) print l; print ""; on = 1; next }
    $0 == e             { on = 0 }
    on                  { next }
                        { print }
  ' "$2" > "$T/out" && cat "$T/out" > "$2"
}

check=0
[ "${1:-}" = "--check" ] && check=1
[ $# -gt 0 ] && [ $check = 0 ] && { sed -n '/^# Usage:/,/^# Copyright/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 1; }

bad=0
while read -r plugin file levels base checked; do
  [ -n "$plugin" ] || continue

  entries "$plugin" "$levels" "$base" > "$T/entries"
  [ -s "$T/entries" ] || { echo "doc/options.md: no entries for the $plugin plugin (## Plugin: $plugin ...)"; bad=1; continue; }

  if [ ! -f "$file" ]; then
    [ "$checked" = yes ] && { echo "$file: not there - the $plugin plugin's options go in it"; bad=1; }
    continue
  fi

  copied "$plugin" "$file" > "$T/copied"
  if [ $? = 2 ]; then
    echo "$file: no '<!-- options of the $plugin plugin: ... -->' marker"; bad=1; continue
  fi
  # the copy is the entries with an empty line before and after
  { echo; cat "$T/entries"; echo; } > "$T/want"

  if [ $check = 1 ]; then
    [ "$checked" = yes ] || continue
    cmp -s "$T/want" "$T/copied" || { echo "$file: the $plugin plugin's options differ from doc/options.md - run tools/pluginOptions.sh"; bad=1; }
  else
    cmp -s "$T/want" "$T/copied" && continue
    write "$plugin" "$file" "$T/entries" && echo "$file: the $plugin plugin's options rewritten"
  fi
done <<< "$COPIES"

[ $check = 1 ] && [ $bad = 0 ] && echo "the plugins' options: every copy as doc/options.md has it"
exit $bad
