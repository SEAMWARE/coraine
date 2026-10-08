#!/usr/bin/env bash
#
# publish.sh <repo-dir> <deb>... - add coraine .debs to a signed apt repository (reprepro)
#
# <repo-dir> is the repository as it is served - in CI, the checked-out GitHub Pages branch; it keeps
# reprepro's conf/ and db/ beside dists/ and pool/, so that every publish ADDS to what is there. A
# package version once in the pool is never replaced: the same version built again with other bytes is
# refused (reprepro: "already registered with different checksums").
#
#   dists   resolute (Ubuntu 26.04), noble (Ubuntu 24.04), trixie (Debian 13) - from the version suffix
#           of each .deb (+ubuntu26.04, +ubuntu24.04, +debian13)
#   comps   main     releases (0.4.0+...)
#           testing  builds of a commit (0.4.0~git<date>.<sha>+...)
#   archs   amd64 arm64
#
# Environment:
#   APT_GPG_PRIVATE_KEY  the signing key, ASCII-armoured, WITHOUT a passphrase - a key (or subkey) made
#                        for this repository and nothing else
#   APT_DOMAIN           optional: a custom domain the repository is served on (a CNAME record to
#                        <owner>.github.io); without it the repository is at Pages' own address,
#                        https://<owner>.github.io/<repo> - e.g. https://seamware.github.io/apt
#
# What a user then does (URL = the repository's address):
#   curl -fsSL $URL/coraine.gpg | sudo tee /usr/share/keyrings/coraine.gpg > /dev/null
#   echo "deb [signed-by=/usr/share/keyrings/coraine.gpg] $URL $(. /etc/os-release; echo $VERSION_CODENAME) main" \
#     | sudo tee /etc/apt/sources.list.d/coraine.list
#   sudo apt update && sudo apt install coraine
#
# Needs reprepro and gnupg.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

REPO=$(mkdir -p "${1:?usage: publish.sh <repo-dir> <deb>...}" && cd "$1" && pwd)
shift
[ $# -gt 0 ] || { echo "publish.sh: no .deb given" >&2; exit 1; }
: "${APT_GPG_PRIVATE_KEY:?the signing key (ASCII-armoured)}"

export GNUPGHOME=$(mktemp -d)
trap 'rm -rf "$GNUPGHOME"' EXIT
chmod 700 "$GNUPGHOME"
printf '%s\n' "$APT_GPG_PRIVATE_KEY" | gpg --batch --quiet --import
FPR=$(gpg --batch --with-colons --list-secret-keys | awk -F: '$1 == "fpr" { print $10; exit }')
[ -n "$FPR" ] || { echo "publish.sh: no secret key in APT_GPG_PRIVATE_KEY" >&2; exit 1; }

#
# conf/ - written every time: the three distributions are this file's, not the repository's
#
mkdir -p "$REPO/conf"
: > "$REPO/conf/distributions"
for d in "resolute:Ubuntu 26.04" "noble:Ubuntu 24.04 LTS" "trixie:Debian 13"; do
  cat >> "$REPO/conf/distributions" <<EOF
Origin: Seamware
Label: coraine
Codename: ${d%%:*}
Architectures: amd64 arm64
Components: main testing
Description: coraine NGSI-LD context broker - ${d#*:}
SignWith: $FPR

EOF
done
printf 'verbose\nask-passphrase\n' > "$REPO/conf/options"

for deb in "$@"; do
  v=$(dpkg-deb -f "$deb" Version)
  case "$v" in
    *+ubuntu26.04) dist=resolute ;;
    *+ubuntu24.04) dist=noble ;;
    *+debian13)    dist=trixie ;;
    *) echo "publish.sh: $deb - no distribution suffix in version $v" >&2; exit 1 ;;
  esac
  case "$v" in *~git*) comp=testing ;; *) comp=main ;; esac
  echo "publish.sh: $(basename "$deb") -> $dist/$comp"
  reprepro -b "$REPO" -C "$comp" includedeb "$dist" "$deb"
done

# the public key, both ways (binary for signed-by=, armoured for reading), and the domain for Pages
gpg --batch --export "$FPR" > "$REPO/coraine.gpg"
gpg --batch --armor --export "$FPR" > "$REPO/coraine.asc"
if [ -n "${APT_DOMAIN:-}" ]; then echo "$APT_DOMAIN" > "$REPO/CNAME"; else rm -f "$REPO/CNAME"; fi
touch "$REPO/.nojekyll"      # Pages serves the tree as it is (no Jekyll: dists/, pool/ ... untouched)

reprepro -b "$REPO" list resolute; reprepro -b "$REPO" list noble; reprepro -b "$REPO" list trixie
