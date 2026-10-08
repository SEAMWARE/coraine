#!/usr/bin/env bash
#
# ci-env.sh - the build and test environment of the Cor stack, installed on a bare Ubuntu runner
#
# What corLibs/docker/Dockerfile.ci puts into the CI image, minus the DDS stack: the build tools, the
# libraries the stack links (libmicrohttpd with TLS and mongo-c-driver from source, the versions the
# image pins), and what the functional tests drive (psql, mongosh, mosquitto, socat, ...). For a runner
# the CI image does not exist for - linux/arm64 today (the image is amd64 only).
#
# Keep it in step with Dockerfile.ci: a package missing here is a test that fails on ARM and nowhere else.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euxo pipefail

MONGOC_VERSION=${MONGOC_VERSION:-2.2.2}
MHD_VERSION=${MHD_VERSION:-1.0.2}

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake git pkg-config ca-certificates curl gnupg \
  libssl-dev libgeos-dev libmosquitto-dev libpq-dev libicu-dev \
  libsasl2-dev libzstd-dev zlib1g-dev libgnutls28-dev \
  python3 coreutils postgresql-client jq procps psmisc openssl mosquitto socat

# mongosh, from MongoDB's own repository (the distribution has none)
. /etc/os-release
curl -fsSL https://pgp.mongodb.com/server-8.0.asc | sudo gpg --dearmor -o /usr/share/keyrings/mongodb.gpg
echo "deb [signed-by=/usr/share/keyrings/mongodb.gpg] https://repo.mongodb.org/apt/ubuntu ${VERSION_CODENAME}/mongodb-org/8.0 multiverse" \
  | sudo tee /etc/apt/sources.list.d/mongodb.list
sudo apt-get update
sudo apt-get install -y --no-install-recommends mongodb-mongosh

# libmicrohttpd WITH TLS (the https tests) - the distribution's is older than the stack wants
curl -fsSL "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-${MHD_VERSION}.tar.gz" -o /tmp/mhd.tar.gz
tar -xzf /tmp/mhd.tar.gz -C /tmp
(cd "/tmp/libmicrohttpd-${MHD_VERSION}" && ./configure --disable-doc --disable-examples && make -j"$(nproc)" && sudo make install)

# mongo-c-driver
git clone --depth 1 --branch "${MONGOC_VERSION}" https://github.com/mongodb/mongo-c-driver.git /tmp/mongo-c
cmake -S /tmp/mongo-c -B /tmp/mongo-c/build -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF -DBUILD_TESTING=OFF
cmake --build /tmp/mongo-c/build -j"$(nproc)"
sudo cmake --install /tmp/mongo-c/build
sudo ldconfig

# where the stack installs - the libs (corLibs bootstrap), the broker (make di) and its plugins: in the CI
# image the build runs as root; on a runner it runs as the runner's user, which then owns them (a runner
# is thrown away after the job)
sudo mkdir -p /opt/seamware/plugins /opt/seamware/etc
sudo chown -R "$(id -u):$(id -g)" /opt/seamware /usr/local

# the same completeness check as the image's
pkg-config --exists mongoc2
pkg-config --exists libmicrohttpd
for cmd in cmake gcc git curl jq python3 psql mongosh pkill pgrep killall timeout openssl mosquitto socat; do
  command -v "$cmd" >/dev/null || { echo "FATAL: $cmd missing"; exit 1; }
done
echo "environment complete: $(uname -m), $(gcc --version | head -1)"
