#!/bin/bash
#
# changelog.sh - the coming release's CHANGELOG section, from the entries in changelog.d/.
#
# A pull request does not edit CHANGELOG.md: it adds its entry as a file of its own, changelog.d/<name>.md
# (changelog.d/README.md says how). Two pull requests never touch the same file, so a merge of one does not
# put the others in conflict - which, with every entry added at the same place in CHANGELOG.md, it did, and
# each conflict resolved was another full CI run. At a release the entries become the version's section
# of CHANGELOG.md, and the files are removed.
#
# An entry file: one or more '### <section>' headings, each followed by its text - '- ' bullets, as in
# CHANGELOG.md. The sections, in the order they are written: New, Faster, Less memory, Changed behaviour,
# Fixed. A section CHANGELOG.md's unreleased part already has under another name keeps its place after them.
#
# The entries are taken in the order they reached main (the commit that added each file), an entry not
# committed yet last. A CHANGELOG.md section headed '## <x> (not released yet)' is the coming release's,
# written before the entries existed: the entries are added to it, after its own text.
#
# Usage:
#   changelog.sh                              the section the release will get, printed; nothing is changed
#   changelog.sh --check                      every entry file well-formed - exit 1 and the reason if not (CI)
#   changelog.sh --release <version> [<date>] CHANGELOG.md gets the section as '## <version> — <date>'
#                                             (default: today), in place of the unreleased one if there is
#                                             one, and the entry files are removed (git rm)
#
# Run from anywhere in the coraine checkout.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -u
set -o pipefail

top=$(git rev-parse --show-toplevel) || exit 1
cd "$top" || exit 1

SECTIONS=("New" "Faster" "Less memory" "Changed behaviour" "Fixed")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

#
# entries - the entry files, in the order they reached main; one not committed yet last
#
entries() {
  local f t
  for f in changelog.d/*.md; do
    [ -f "$f" ] || continue
    [ "$f" = changelog.d/README.md ] && continue
    t=$(git log --diff-filter=A --format=%ct -1 -- "$f" 2>/dev/null)
    echo "${t:-9999999999} $f"
  done | sort -k1,1n -k2 | cut -d' ' -f2-
}

#
# known <section> - one of SECTIONS
#
known() {
  local s
  for s in "${SECTIONS[@]}"; do [ "$s" = "$1" ] && return 0; done
  return 1
}

#
# check - every entry file: text only under a '### <known section>', and at least one '- ' bullet
#
check() {
  local f bad=0 out
  for f in changelog.d/*; do
    case "$f" in
      *[[:space:]]*) echo "'$f': a space in the name"; bad=1 ;;
      *.md)          ;;
      *)             [ -e "$f" ] && { echo "$f: not a .md file"; bad=1; } ;;
    esac
  done
  [ $bad = 0 ] || return 1
  for f in $(entries); do
    out=$(awk '
      /^### /                 { s = substr($0, 5); print "S " s; next }
      /^[[:space:]]*$/        { next }
      s == ""                 { print "E text before the first ### heading: " $0; exit }
      /^- /                   { print "B" }
    ' "$f")
    if echo "$out" | grep -q '^E '; then
      echo "$f: $(echo "$out" | sed -n 's/^E //p')"; bad=1; continue
    fi
    if ! echo "$out" | grep -q '^S '; then
      echo "$f: no '### <section>' heading"; bad=1; continue
    fi
    while read -r s; do
      known "$s" || { echo "$f: '### $s' is none of: ${SECTIONS[*]}"; bad=1; }
    done < <(echo "$out" | sed -n 's/^S //p')
    echo "$out" | grep -q '^B$' || { echo "$f: no '- ' entry"; bad=1; }
  done
  mentions changelog.d/*.md CHANGELOG.md || bad=1
  return $bad
}

#
# mentions <file>... - an '@word' outside a code span is a GitHub @mention in the release notes: its user is
# listed as the release's contributor (v0.5.0 named the user "context", from a bare @context). Written
# `@context`, it is not.
#
mentions() {
  local out
  out=$(awk '
    FILENAME ~ /README\.md$/ { next }
    {
      line = $0
      gsub(/`[^`]*`/, "", line)
      if (match(line, /(^|[^A-Za-z0-9_.\/-])@[A-Za-z][A-Za-z0-9-]*/))
        print FILENAME ":" FNR ": " substr(line, RSTART, RLENGTH) " outside `` - write it `@...`, or GitHub takes it for a user"
    }
  ' "$@" 2>/dev/null)
  [ -z "$out" ] && return 0
  echo "$out"
  return 1
}

#
# split <file> <prefix> - appends each '### <section>' part of <file> to $T/<prefix><n> (n: the section's
# index in $T/names, added there if new); the text before the first heading to $T/<prefix>pre. Leading
# and trailing empty lines of each part are dropped.
#
split() {
  awk -v T="$T" -v p="$2" '
    function flush(   i, a, b) {
      if (n == 0) return
      for (a = 1; a <= n && buf[a] ~ /^[[:space:]]*$/; a++) ;
      for (b = n; b >= a && buf[b] ~ /^[[:space:]]*$/; b--) ;
      for (i = a; i <= b; i++) print buf[i] >> out
      close(out)
      n = 0
    }
    function index_of(s,   i, line) {
      i = 0
      while ((getline line < (T "/names")) > 0) { i++; if (line == s) { close(T "/names"); return i } }
      close(T "/names")
      print s >> (T "/names"); close(T "/names")
      return i + 1
    }
    BEGIN       { out = T "/" p "pre" }
    /^### /     { flush(); out = T "/" p index_of(substr($0, 5)); next }
                { buf[++n] = $0 }
    END         { flush() }
  ' "$1"
}

#
# compose - the section's text (under the '## ' heading) on stdout: the unreleased part of CHANGELOG.md with
# the entries added. Sets UNRELEASED to that part's heading line ('' if none).
#
compose() {
  local s i f any=0
  UNRELEASED=$(awk '/^## / { if (/\(not released yet\)/) print; exit }' CHANGELOG.md)

  : > "$T/names"
  for s in "${SECTIONS[@]}"; do echo "$s" >> "$T/names"; done

  if [ -n "$UNRELEASED" ]; then
    awk -v h="$UNRELEASED" '
      $0 == h         { on = 1; next }
      on && /^## /    { exit }
      on && /^---$/   { next }
      on              { print }
    ' CHANGELOG.md > "$T/unreleased"
    split "$T/unreleased" c
  fi
  for f in $(entries); do
    split "$f" e
  done

  [ -s "$T/cpre" ] && { cat "$T/cpre"; echo; }
  i=0
  while read -r s; do
    i=$((i + 1))
    [ -s "$T/c$i" ] || [ -s "$T/e$i" ] || continue
    echo "### $s"
    echo
    [ -s "$T/c$i" ] && cat "$T/c$i"
    [ -s "$T/e$i" ] && cat "$T/e$i"
    echo
    any=1
  done < "$T/names"
  [ "$any" = 1 ]
}

case "${1:-}" in
  "")
    check || exit 1
    compose || { echo "changelog.sh: nothing for the coming release - no entry in changelog.d/" >&2; exit 1; }
    ;;

  --check)
    check || exit 1
    echo "changelog.d: $(entries | grep -c .) entry file(s), all well-formed"
    ;;

  --release)
    version="${2:?usage: changelog.sh --release <version> [<date>]}"
    date="${3:-$(date +%F)}"
    #
    # A released section of this version refuses - '## <version> (not released yet)' is the one to replace
    #
    grep "^## ${version//./\\.}\( \|$\)" CHANGELOG.md | grep -qv '(not released yet)' \
      && { echo "changelog.sh: CHANGELOG.md has a '## $version' section already" >&2; exit 1; }
    check || exit 1
    compose > "$T/section" || { echo "changelog.sh: nothing to release - no entry in changelog.d/ and no unreleased section" >&2; exit 1; }

    #
    # The header (up to the first '## '), the new section, the rest - from the first release section on
    #
    awk -v h="$UNRELEASED" -v v="## $version — $date" -v f="$T/section" '
      !done && /^## / {
        print v; print ""
        while ((getline line < f) > 0) print line
        print "---"; print ""
        done = 1
        if ($0 == h) { skip = 1; next }
      }
      skip && /^## /  { skip = 0 }
      skip            { next }
                      { print }
    ' CHANGELOG.md > "$T/CHANGELOG.md" || exit 1
    cp "$T/CHANGELOG.md" CHANGELOG.md

    files=$(entries)
    [ -n "$files" ] && git rm -q $files
    echo "CHANGELOG.md: '## $version — $date' written, $(echo "$files" | grep -c .) entries folded in and removed"
    echo "check it (git diff CHANGELOG.md), then commit both"
    ;;

  *)
    sed -n '/^# Usage:/,/^# Run from/p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
