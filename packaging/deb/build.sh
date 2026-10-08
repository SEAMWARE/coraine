#!/usr/bin/env bash
#
# build.sh <coraine-src.tar.xz> <out-dir> - the packages coraine and coraine-dev, for THIS distribution
# and architecture
#
# Runs as root INSIDE a throw-away container of the target distribution (it installs the build
# dependencies with apt): Ubuntu 26.04, Ubuntu 24.04 or Debian 13, amd64 or arm64.
#
#   docker run --rm -v $PWD:/w -w /w ubuntu:26.04 packaging/deb/build.sh dist/coraine-src.tar.xz dist
#
# The tarball is source.sh's. The broker is built with coraine-build - the command coraine-dev ships -
# from that tarball, so every package build also proves that coraine-build works.
#
# Out: coraine_<version>_<arch>.deb, coraine-dev_<version>_<arch>.deb, coraine-dbg_<version>_<arch>.tar.xz
# (the stripped debug info, usr/lib/debug/.build-id/..: unpacked in / gdb finds it) and
# lintian-<dist>-<arch>.txt. <version> = <upstream>+<dist>: one pool can hold all three distributions.
#
#   BUILD_KIND      pgo (default - what the image ships) | release (no training: quicker)
#   MONGOC_VERSION  the mongo-c driver bundled where the distribution has no v2 (docker/Dockerfile's)
#   DEB_MAINTAINER  the Maintainer field
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

SRC=$(realpath "${1:?usage: build.sh <coraine-src.tar.xz> <out-dir>}")
OUT=$(mkdir -p "${2:?usage: build.sh <coraine-src.tar.xz> <out-dir>}" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD_KIND=${BUILD_KIND:-pgo}
MONGOC_VERSION=${MONGOC_VERSION:-2.2.2}
MAINTAINER=${DEB_MAINTAINER:-Ken Zangelin <kzangeli@gmail.com>}

die() { echo "build.sh: $*" >&2; exit 1; }

#
# Which distribution: the version suffix, and whether mongo-c v2 comes from it or is bundled
#
#   Ubuntu 26.04  libmongoc 2.2.2   -> Depends on it
#   Ubuntu 24.04  libmongoc 1.26    -> v2 built here, bundled in /opt/seamware/lib (the system's 1.x untouched)
#   Debian 13     libmongoc 1.30    -> the same
#
. /etc/os-release
case "$ID$VERSION_ID" in
  ubuntu26.04) DIST=ubuntu26.04; BUNDLE=0 ;;
  ubuntu24.04) DIST=ubuntu24.04; BUNDLE=1 ;;
  debian13)    DIST=debian13;    BUNDLE=1 ;;
  *)           die "not a distribution packaged for: $PRETTY_NAME" ;;
esac
CODENAME=$VERSION_CODENAME
ARCH=$(dpkg --print-architecture)

#
# Build dependencies - the coraine-dev Depends below are the same list
#
export DEBIAN_FRONTEND=noninteractive
DEV_DEPS="build-essential cmake pkgconf python3 curl wrk xz-utils libssl-dev libgeos-dev libmosquitto-dev libpq-dev libmicrohttpd-dev libzstd-dev libsasl2-dev zlib1g-dev"
if [ "$BUNDLE" = 0 ]; then DEV_DEPS="$DEV_DEPS libmongoc-dev"; else DEV_DEPS="$DEV_DEPS patchelf"; fi
apt-get update
apt-get install -y --no-install-recommends $DEV_DEPS git ca-certificates file patchelf dpkg-dev lintian binutils

WORK=/tmp/coraine-deb
rm -rf "$WORK"
mkdir -p "$WORK"
tar -xJf "$SRC" -C "$WORK" --wildcards coraine-*/SOURCE coraine-*/MANIFEST.txt
. "$WORK"/coraine-*/SOURCE          # version, gitSha, commitDate
UPSTREAM=$version
DEBVER=$UPSTREAM+$DIST
echo "build.sh: coraine $DEBVER ($ARCH, $BUILD_KIND) from ${gitSha:0:8}"

#
# mongo-c driver v2, where the distribution has only v1: into /opt/seamware (lib, include), every library
# with RUNPATH $ORIGIN - libmongoc2 finds libbson2 beside it, never a system one
#
if [ "$BUNDLE" = 1 ]; then
  git clone -q --depth 1 --branch "$MONGOC_VERSION" https://github.com/mongodb/mongo-c-driver.git "$WORK/mongo-c"
  cmake -S "$WORK/mongo-c" -B "$WORK/mongo-c/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/seamware -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_RPATH='$ORIGIN' \
        -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_STATIC=OFF \
        -DENABLE_SSL=OPENSSL -DENABLE_SASL=CYRUS -DENABLE_ZSTD=ON -DENABLE_ZLIB=SYSTEM -DENABLE_SNAPPY=OFF \
        -DENABLE_CLIENT_SIDE_ENCRYPTION=OFF -DENABLE_SRV=ON -DENABLE_MAN_PAGES=OFF -DENABLE_HTML_DOCS=OFF \
        -DENABLE_UNINSTALL=OFF > "$WORK/mongo-c.log"
  cmake --build "$WORK/mongo-c/build" -j"$(nproc)" >> "$WORK/mongo-c.log"
  cmake --install "$WORK/mongo-c/build" >> "$WORK/mongo-c.log"
  pkg-config --exists mongoc2 && die "a system mongoc2 as well as the bundled one - which would the build take?"
  PKG_CONFIG_PATH=/opt/seamware/lib/pkgconfig pkg-config --modversion mongoc2
fi

#
# The broker, its plugins, coraine-import - with coraine-build, from the tarball
#
"$HERE/coraine-build" --source "$SRC" --"$BUILD_KIND" "$WORK/tree"
INST=$WORK/tree/install

# ------------------------------------------------------------------------------------------------- #
# coraine
# ------------------------------------------------------------------------------------------------- #
S=$WORK/stage/coraine
install -d "$S/DEBIAN" "$S/usr/bin" "$S/opt/seamware/etc" "$S/usr/lib/systemd/system" "$S/etc/default" \
           "$S/usr/share/doc/coraine" "$S/usr/share/lintian/overrides"
install -m 0755 "$INST/bin/coraine" "$INST/bin/coraine-import" "$S/usr/bin/"
cp -r "$INST/plugins" "$S/opt/seamware/plugins"
install -m 0644 "$INST/etc/contextSourceExtras.json" "$S/opt/seamware/etc/"
install -m 0644 "$HERE/coraine/coraine.service" "$S/usr/lib/systemd/system/coraine.service"
install -m 0644 "$HERE/coraine/default"         "$S/etc/default/coraine"
install -m 0755 "$HERE/coraine/postinst" "$HERE/coraine/prerm" "$HERE/coraine/postrm" "$S/DEBIAN/"
echo /etc/default/coraine > "$S/DEBIAN/conffiles"
install -m 0644 "$WORK"/coraine-*/MANIFEST.txt "$S/usr/share/doc/coraine/MANIFEST.txt"

if [ "$BUNDLE" = 1 ]; then
  install -d "$S/opt/seamware/lib" "$S/usr/share/doc/coraine/mongo-c-driver"
  cp -P /opt/seamware/lib/libmongoc2.so.* /opt/seamware/lib/libbson2.so.* "$S/opt/seamware/lib/"
  for f in COPYING THIRD_PARTY_NOTICES; do
    [ ! -f "$WORK/mongo-c/$f" ] || install -m 0644 "$WORK/mongo-c/$f" "$S/usr/share/doc/coraine/mongo-c-driver/"
  done
  #
  # The one binary that links libmongoc2: coraine-build gave it RUNPATH /opt/seamware/lib. Check, here
  # rather than at the first `--database mongoc` on somebody's machine.
  #
  readelf -d "$S/opt/seamware/plugins/db/currentState/mongoc.so" | grep -q 'RUNPATH.*\[/opt/seamware/lib\]' \
    || die "mongoc.so has no RUNPATH /opt/seamware/lib"
fi

cat > "$S/usr/share/lintian/overrides/coraine" <<EOF
# /opt/seamware is the broker's compiled-in layout (plugins, etc; SEAMWARE_PLUGIN_DIR) - the same as the
# image and a source install; the bundled mongo-c v2 lives beside it, apart from the system's v1
coraine: dir-or-file-in-opt
# a daemon; coraine -U and doc/installation.md are its manual
coraine: no-manual-page
# a plugin that calls only into the broker (which dlopens it; its symbols come from the broker's
# -rdynamic) links nothing, not even libc
coraine: shared-library-lacks-prerequisites
EOF
if [ "$BUNDLE" = 1 ]; then
  cat >> "$S/usr/share/lintian/overrides/coraine" <<EOF
# mongoc.so finds the bundled libmongoc2 in /opt/seamware/lib through its RUNPATH
coraine: custom-library-search-path
EOF
fi

# ------------------------------------------------------------------------------------------------- #
# coraine-dev
# ------------------------------------------------------------------------------------------------- #
D=$WORK/stage/coraine-dev
install -d "$D/DEBIAN" "$D/usr/bin" "$D/usr/share/coraine/src" "$D/usr/share/doc/coraine-dev" "$D/usr/share/lintian/overrides"
install -m 0755 "$HERE/coraine-build" "$D/usr/bin/coraine-build"
install -m 0644 "$SRC" "$D/usr/share/coraine/src/coraine-src.tar.xz"
install -m 0644 "$WORK"/coraine-*/MANIFEST.txt "$D/usr/share/doc/coraine-dev/MANIFEST.txt"
if [ "$BUNDLE" = 1 ]; then
  # the bundled driver's development half: headers, the .so links, pkg-config and CMake files
  install -d "$D/opt/seamware/lib"
  cp -r /opt/seamware/include "$D/opt/seamware/include"
  cp -P /opt/seamware/lib/libmongoc2.so /opt/seamware/lib/libbson2.so "$D/opt/seamware/lib/"
  cp -r /opt/seamware/lib/pkgconfig "$D/opt/seamware/lib/"
  [ ! -d /opt/seamware/lib/cmake ] || cp -r /opt/seamware/lib/cmake "$D/opt/seamware/lib/"
fi
cat > "$D/usr/share/lintian/overrides/coraine-dev" <<EOF
# a shell script; coraine-build --help is its manual
coraine-dev: no-manual-page
EOF
if [ "$BUNDLE" = 1 ]; then
  cat >> "$D/usr/share/lintian/overrides/coraine-dev" <<EOF
# the bundled mongo-c v2 headers and links, beside its libraries in coraine
coraine-dev: dir-or-file-in-opt
EOF
else
  cat >> "$D/usr/share/lintian/overrides/coraine-dev" <<EOF
# only the source tarball and a script here; not Architecture: all, because on Ubuntu 24.04 and Debian 13
# it holds the bundled driver - one package shape for every distribution
coraine-dev: package-contains-no-arch-dependent-files
EOF
fi

# ------------------------------------------------------------------------------------------------- #
# both: copyright, changelog, permissions
# ------------------------------------------------------------------------------------------------- #
LICENSE_TEXT=" On Debian systems, the full text of the Apache License, Version 2.0 can be found
 in /usr/share/common-licenses/Apache-2.0."
COMMIT_DATE_R=$(date -R -d "$commitDate")

for p in coraine coraine-dev; do
  P=$WORK/stage/$p
  {
    echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    echo "Upstream-Name: coraine"
    echo "Upstream-Contact: https://github.com/SEAMWARE/coraine/issues"
    echo "Source: https://github.com/SEAMWARE/coraine"
    echo "Comment: coraine and the Cor-Libs (github.com/SEAMWARE) at the commits in MANIFEST.txt"
    echo
    echo "Files: *"
    echo "Copyright: 2026 Seamware"
    echo "License: Apache-2.0"
    if [ "$BUNDLE" = 1 ]; then
      echo
      echo "Files: opt/seamware/lib/*mongoc2* opt/seamware/lib/*bson2* opt/seamware/include/* opt/seamware/lib/pkgconfig/* opt/seamware/lib/cmake/*"
      echo "Copyright: MongoDB, Inc."
      echo "License: Apache-2.0"
      echo "Comment: the MongoDB C driver $MONGOC_VERSION (https://github.com/mongodb/mongo-c-driver), built from"
      echo " source. Its third-party notices: /usr/share/doc/coraine/mongo-c-driver/THIRD_PARTY_NOTICES"
    fi
    echo
    echo "License: Apache-2.0"
    echo "$LICENSE_TEXT"
  } > "$P/usr/share/doc/$p/copyright"

  {
    echo "$p ($DEBVER) $CODENAME; urgency=medium"
    echo
    echo "  * coraine $UPSTREAM, built from commit ${gitSha:0:12}."
    echo "    Every Cor-Lib's commit: /usr/share/doc/$p/MANIFEST.txt."
    echo "    The changes: CHANGELOG.md in the source."
    echo
    echo " -- $MAINTAINER  $COMMIT_DATE_R"
  } | gzip -9n > "$P/usr/share/doc/$p/changelog.gz"
done

#
# Strip, keeping the debug info apart (usr/lib/debug/.build-id/xx/yyyy.debug, where gdb looks): a crash
# report names a build id, and the debug tarball of the same build turns its addresses into lines.
#
DBG=$WORK/stage/dbg
for f in $(find "$S" "$D" -type f); do
  file -b "$f" | grep -q '^ELF' || continue
  id=$(readelf -n "$f" | sed -n 's/.*Build ID: \([0-9a-f]*\).*/\1/p' | head -1)
  if [ -n "$id" ]; then
    install -d "$DBG/usr/lib/debug/.build-id/${id:0:2}"
    objcopy --only-keep-debug --compress-debug-sections "$f" "$DBG/usr/lib/debug/.build-id/${id:0:2}/${id:2}.debug"
  fi
  strip --strip-unneeded --remove-section=.comment --remove-section=.note "$f"
done
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$DBG" -cJf "$OUT/coraine-dbg_${DEBVER}_${ARCH}.tar.xz" .

find "$S" "$D" -type d -exec chmod 0755 {} +
find "$S/opt" -type f -exec chmod 0644 {} +
[ ! -d "$D/opt" ] || find "$D/opt" -type f -exec chmod 0644 {} +

#
# coraine's Depends: what its ELF files link, from dpkg-shlibdeps (the bundled libmongoc2/libbson2 are
# the package's own and have no shlibs entry - --ignore-missing-info). Plus, by hand: adduser (postinst)
# and libzstd1, which corDB loads with dlopen for --dbCompress.
#
mkdir -p "$WORK/shl/debian"
printf 'Source: coraine\n\nPackage: coraine\nArchitecture: any\n' > "$WORK/shl/debian/control"
ELFS=$(for f in $(find "$S" -type f); do if file -b "$f" | grep -q '^ELF'; then echo "-e$f"; fi; done)
SHLIBS=$(cd "$WORK/shl" && dpkg-shlibdeps -O --warnings=0 --ignore-missing-info -l"$S/opt/seamware/lib" $ELFS \
         | sed -n 's/^shlibs:Depends=//p')
[ -n "$SHLIBS" ] || die "dpkg-shlibdeps found no dependencies"

cat > "$S/DEBIAN/control" <<EOF
Package: coraine
Version: $DEBVER
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $(du -sk --exclude=DEBIAN "$S" | cut -f1)
Depends: $SHLIBS, adduser$(echo "$SHLIBS" | grep -q 'libzstd1' || printf ', libzstd1')
Recommends: ca-certificates
Section: net
Priority: optional
Homepage: https://github.com/SEAMWARE/coraine
Description: NGSI-LD context broker (ETSI GS CIM 009)
 coraine is an NGSI-LD context broker: one binary and the plugins it loads at
 startup. With its own store, corDB (in memory, and on disk with --dbDir), it
 needs no external service; MongoDB and TimescaleDB are options.
 .
 This package holds the broker, coraine-import (moves another broker's
 database into coraine's stores), the plugins - corDB, ramDB, mongoc,
 timescale, the admin API, the WebSocket transport, the MQTT and Modbus
 bridges - and the systemd unit coraine.service, installed but not enabled:
 see /etc/default/coraine.$( [ "$BUNDLE" = 0 ] || printf '\n .\n The MongoDB C driver v2 is bundled, privately, in /opt/seamware/lib.' )
EOF

cat > "$D/DEBIAN/control" <<EOF
Package: coraine-dev
Version: $DEBVER
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $(du -sk --exclude=DEBIAN "$D" | cut -f1)
Depends: coraine (= $DEBVER), $(echo $DEV_DEPS | sed 's/ /, /g')
Section: devel
Priority: optional
Homepage: https://github.com/SEAMWARE/coraine
Description: NGSI-LD context broker (ETSI GS CIM 009) - build your own
 The exact source of the coraine package of the same version - coraine and
 every Cor-Lib at the commits it was built from - in
 /usr/share/coraine/src, the toolchain and libraries to build it, and
 coraine-build: unpacks that source into a directory and builds a broker
 there, with a choice of features, profile-guided or tuned to a workload,
 release or debug.
EOF

#
# Build, check
#
DEBS=
for p in coraine coraine-dev; do
  deb=$OUT/${p}_${DEBVER}_${ARCH}.deb
  (cd "$WORK/stage/$p" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' | sort -z | xargs -0 md5sum > DEBIAN/md5sums)
  dpkg-deb --root-owner-group -Zxz --build "$WORK/stage/$p" "$deb" > /dev/null
  DEBS="$DEBS $deb"
  echo "build.sh: $(basename "$deb") ($(du -h "$deb" | cut -f1))"
done

dpkg-deb --info "$OUT/coraine_${DEBVER}_${ARCH}.deb" | sed -n '/Depends/p'
LINTIAN_OUT=$OUT/lintian-$DIST-$ARCH.txt
set +e
lintian --allow-root --tag-display-limit 0 --display-info --fail-on error $DEBS > "$LINTIAN_OUT" 2>&1
rc=$?
set -e
echo "build.sh: lintian ($(lintian --version)) - exit $rc:"
cat "$LINTIAN_OUT"
[ $rc = 0 ] || die "lintian reports errors - see above"
