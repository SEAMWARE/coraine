#!/usr/bin/env bash
#
# test-dev.sh <dir> - install coraine-dev (and coraine) from <dir>, build a reduced broker with
# coraine-build as an ordinary user, and run it
#
# As root, in a fresh container of the distribution the packages were built for:
#
#   docker run --rm -v $PWD/dist:/debs debian:trixie /debs/test-dev.sh /debs
#
# The build runs as a user that cannot write outside its home: coraine-build must need nothing else.
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

DIR=$(cd "${1:?usage: test-dev.sh <dir-with-debs>}" && pwd)
FEATURES=REGISTRATIONS=OFF,SERVICE_EXECUTION=OFF,MIGRATE=OFF
PORT=1031
fail() { echo "test-dev: FAIL - $*" >&2; [ ! -f /tmp/built.log ] || { echo "--- broker log:"; tail -50 /tmp/built.log; } >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
echo "test-dev: $PRETTY_NAME, $(dpkg --print-architecture)"

apt-get update -qq
apt-get install -y --no-install-recommends "$(ls "$DIR"/coraine_*.deb)" "$(ls "$DIR"/coraine-dev_*.deb)" > /tmp/apt.log \
  || { cat /tmp/apt.log; fail "apt-get install"; }
dpkg -s coraine-dev | sed -n '/^Version/p;/^Depends/p'

useradd -m builder
runuser -u builder -- coraine-build --list-features > /tmp/features.txt; head -5 /tmp/features.txt
time runuser -u builder -- coraine-build --features "$FEATURES" /home/builder/b > /tmp/build.log 2>&1 \
  || { tail -60 /tmp/build.log; fail "coraine-build"; }
tail -25 /tmp/build.log

B=/home/builder/b/install
v=$(runuser -u builder -- "$B/bin/coraine" --version)
echo "$v"
for f in REGISTRATIONS SERVICE_EXECUTION MIGRATE; do
  echo "$v" | grep -q " $f=0" || fail "$f is not off in the broker built"
done
[ ! -e "$B/bin/coraine-import" ] || fail "MIGRATE=OFF and yet coraine-import"

runuser -u builder -- bash -c ". $B/env && coraine --database mongoc -u" > /tmp/u.log 2>&1 || { cat /tmp/u.log; fail "the mongoc.so built does not load"; }
echo "the mongoc plugin built: loads"

setpriv --reuid=builder --regid=builder --init-groups -- bash -c ". $B/env && exec coraine -fg --port $PORT --database corDB" > /tmp/built.log 2>&1 &
PID=$!
for i in $(seq 1 50); do
  curl -sf "localhost:$PORT/version" > /dev/null && break
  kill -0 $PID 2>/dev/null || fail "the broker built exited at start"
  [ "$i" = 50 ] && fail "the broker built never answered"
  sleep 0.2
done
curl -sf "localhost:$PORT/version"; echo
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "localhost:$PORT/ngsi-ld/v1/entities" -H 'Content-Type: application/json' \
       -d '{"id":"urn:ngsi-ld:T:dev1","type":"T","p":{"type":"Property","value":1}}')
[ "$code" = 201 ] || fail "create answered $code"
curl -sf "localhost:$PORT/ngsi-ld/v1/entities/urn:ngsi-ld:T:dev1" | grep -q '"value":1' || fail "retrieve"
kill -TERM $PID; wait $PID || fail "the broker built did not exit 0 on SIGTERM"

echo
echo "test-dev: PASS - coraine-build --features $FEATURES, as an ordinary user, started and answered"
