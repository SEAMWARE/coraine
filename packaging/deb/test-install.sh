#!/usr/bin/env bash
#
# test-install.sh <dir> - install coraine_*.deb from <dir> into THIS (clean) system and run it
#
# As root, in a fresh container of the distribution the package was built for - nothing but the image:
#
#   docker run --rm -v $PWD/dist:/debs ubuntu:24.04 /debs/test-install.sh /debs
#
#   1. apt installs the .deb - every dependency resolves from the distribution
#   2. every ELF file in it resolves every library it needs (ldd), mongoc.so the bundled driver's
#   3. the systemd unit passes systemd-analyze verify; the user and the state directory exist
#   4. the broker started as the unit starts it (user coraine, /etc/default/coraine, -fg) answers
#      GET /version, creates and retrieves an entity; stopped with SIGTERM it exits 0, and started again
#      it still has the entity (corDB on disk, /var/lib/coraine/db)
#   5. every plugin loads: the DB plugins, the temporal ones, the API plugin, the bridges, the transport
#   6. remove and purge leave no broker behind (and the data, by design)
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -euo pipefail

DIR=$(cd "${1:?usage: test-install.sh <dir-with-debs>}" && pwd)
PORT=1026
fail() { echo "test-install: FAIL - $*" >&2; [ ! -f /tmp/coraine.log ] || { echo "--- broker log:"; tail -50 /tmp/coraine.log; } >&2; exit 1; }
step() { echo; echo "=== $*"; }

export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
echo "test-install: $PRETTY_NAME, $(dpkg --print-architecture)"

step "1. apt install ./coraine_*.deb"
apt-get update -qq
deb=$(ls "$DIR"/coraine_*.deb)
apt-get install -y --no-install-recommends "$deb" curl systemd > /tmp/apt.log || { cat /tmp/apt.log; fail "apt-get install"; }
dpkg -s coraine | sed -n '/^Version/p;/^Depends/p'

step "2. every library resolves"
for f in /usr/bin/coraine /usr/bin/coraine-import $(find /opt/seamware -type f -name '*.so*'); do
  if ldd "$f" | grep -q 'not found'; then ldd "$f"; fail "$f: a library is not found"; fi
done
if [ -f /opt/seamware/lib/libmongoc2.so.2 ]; then
  ldd /opt/seamware/plugins/db/currentState/mongoc.so | grep -E 'libmongoc2|libbson2'
  ldd /opt/seamware/plugins/db/currentState/mongoc.so | grep -q '/opt/seamware/lib/libmongoc2' || fail "mongoc.so does not take the bundled libmongoc2"
  ldconfig -p | grep -q libmongoc2 && fail "the bundled libmongoc2 is visible system-wide"
fi
ldd /usr/bin/coraine | grep -E 'microhttpd|ssl'
coraine --version

step "3. the unit, the user, the state directory"
systemd-analyze verify /usr/lib/systemd/system/coraine.service 2>&1 | grep -v 'Failed to .*\(bus\|D-Bus\)' | tee /tmp/verify.log || true
grep -qi 'error\|invalid\|unknown' /tmp/verify.log && fail "systemd-analyze verify"
getent passwd coraine || fail "no user coraine"
[ "$(stat -c %U /var/lib/coraine)" = coraine ] || fail "/var/lib/coraine is not the user coraine's"
systemctl is-enabled coraine.service 2>/dev/null | grep -q '^enabled' && fail "the service is enabled on install"
cat /etc/default/coraine | grep -v '^#' | grep .

#
# As the unit runs it: the environment file, the user, the working directory, -fg (ExecStart)
#
EXEC=$(sed -n 's/^ExecStart=//p' /usr/lib/systemd/system/coraine.service)
start()
{
  ( cd /var/lib/coraine && set -a && . /etc/default/coraine && set +a && exec setpriv --reuid=coraine --regid=coraine --init-groups -- $EXEC ) >> /tmp/coraine.log 2>&1 &
  PID=$!
  for i in $(seq 1 50); do
    curl -sf "localhost:$PORT/version" > /dev/null && return 0
    kill -0 $PID 2>/dev/null || fail "the broker exited at start"
    sleep 0.2
  done
  fail "the broker never answered GET /version"
}
stop()
{
  kill -TERM $PID
  local rc=0
  wait $PID || rc=$?
  [ $rc = 0 ] || fail "the broker exited $rc on SIGTERM"
}

step "4. the broker, as the service runs it"
start
curl -sf "localhost:$PORT/version"; echo
J='Content-Type: application/json'
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "localhost:$PORT/ngsi-ld/v1/entities" -H "$J" \
       -d '{"id":"urn:ngsi-ld:T:deb1","type":"T","temperature":{"type":"Property","value":21.5}}')
[ "$code" = 201 ] || fail "create answered $code"
curl -sf "localhost:$PORT/ngsi-ld/v1/entities/urn:ngsi-ld:T:deb1" | tee /tmp/e1.json; echo
grep -q '"value":21.5' /tmp/e1.json || fail "retrieve did not give the entity back"
stop
echo "stopped (SIGTERM, exit 0)"
ls -la /var/lib/coraine/db
start
curl -sf "localhost:$PORT/ngsi-ld/v1/entities/urn:ngsi-ld:T:deb1" | grep -q '"value":21.5' || fail "the entity did not survive a restart"
echo "the entity survived the restart"
stop

step "5. every plugin loads"
for db in corDB ramDB mongoc; do
  coraine --database $db -u > /tmp/u.log 2>&1 || { cat /tmp/u.log; fail "--database $db"; }
  echo "--database $db: loads"
done
for t in none timescale ramDB; do
  coraine --database corDB --troe $t -u > /tmp/u.log 2>&1 || { cat /tmp/u.log; fail "--troe $t"; }
  echo "--troe $t: loads"
done
coraine --database corDB --apiPlugins admin -u > /tmp/u.log 2>&1 || { cat /tmp/u.log; fail "--apiPlugins admin"; }
echo "--apiPlugins admin: loads"
for p in bridge/mqtt.so bridge/modbus.so bridge/loopback.so transport/ws.so; do
  [ -f "/opt/seamware/plugins/$p" ] || fail "no plugin $p"
done
echo "bridges mqtt, modbus, loopback and the ws transport: present (and resolved, step 2)"
[ ! -e /opt/seamware/plugins/bridge/dds.so ] || fail "the DDS bridge is in the package"

step "6. remove, purge"
apt-get remove -y coraine > /dev/null
[ ! -e /usr/bin/coraine ] || fail "remove left /usr/bin/coraine"
apt-get purge -y coraine > /dev/null
[ ! -e /etc/default/coraine ] || fail "purge left /etc/default/coraine"
[ -d /var/lib/coraine/db ] || fail "purge took the data (it must not)"
echo "removed and purged; /var/lib/coraine kept"

echo
echo "test-install: PASS - $(basename "$deb")"
