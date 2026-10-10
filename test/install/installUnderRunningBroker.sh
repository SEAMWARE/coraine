#!/bin/bash
#
# FILE            installUnderRunningBroker.sh
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# installUnderRunningBroker.sh - `make install` of a new build while a broker of the old one runs
#
#   test/install/installUnderRunningBroker.sh [--build <dir>] [--port <port>] [--keep]
#
#   --build   the build tree to install from (default BUILD_DEBUG) - built already
#   --port    the broker's port (default 19048)
#   --keep    leave the scratch install directory and the broker's log
#
# A running broker has its plugins mapped (dlopen). An install that writes an installed plugin IN
# PLACE (cp: the same inode, truncated and rewritten) changes the code under that broker, and it dies
# within seconds - SIGSEGV or SIGILL in whichever thread next runs plugin code: corPeriodic, a request
# worker, or the shutdown. The install must put a NEW file in place (copy beside, rename over); the
# running broker keeps the old one, the next start loads the new.
#
# The steps, all in a scratch directory (nothing under /usr/local or /opt is touched):
#
#   1. install the build (make install_debug, PREFIX/PLUGIN_DIR/ETC_DIR in the scratch directory),
#      plus corDB's ramDB.so from the sibling corDB as the store
#   2. start that broker (-db ramDB), create an entity
#   3. install "the next build": the same tree with the plugins' contents changed (none.so and
#      timescale.so swapped - bytes that differ from what the broker has mapped, as a rebuild's do)
#   4. check: the install succeeded (a `cp` onto the running coraine fails: Text file busy), every
#      installed file is a new inode, the broker still answers, and it stops cleanly on SIGTERM -
#      exit 0, no crash report in its log (the shutdown runs the troe plugin's close: none.so)
#
# Exit code: the number of failed checks.
#
BUILD=BUILD_DEBUG
PORT=19048
KEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD=$2; shift ;;
    --port)  PORT=$2; shift ;;
    --keep)  KEEP=1 ;;
    *)       echo "usage: $0 [--build <dir>] [--port <port>] [--keep]"; exit 1 ;;
  esac
  shift
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SIBLING_DIR=${SIBLING_DIR:-$(cd "$REPO/.." && pwd)}
BUILD=$(cd "$REPO" && cd "$BUILD" 2>/dev/null && pwd) || { echo "no build tree: $BUILD"; exit 1; }
RAMDB=$(ls -t "$SIBLING_DIR"/corDB/obj/*/ramDB.so 2>/dev/null | head -1)

[ -x "$BUILD/src/app/coraine/coraine" ] || { echo "no broker in $BUILD - build it first"; exit 1; }
[ -n "$RAMDB" ]                         || { echo "no ramDB.so under $SIBLING_DIR/corDB/obj - build corDB first"; exit 1; }
ss -ltn | grep -q ":$PORT " && { echo "port $PORT busy"; exit 1; }

T=$(mktemp -d /tmp/installUnderRunningBroker.XXXXXX)
FAILS=0
BPID=

fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
ok()   { echo "ok:   $*"; }

cleanup()
{
  [ -n "$BPID" ] && kill -0 "$BPID" 2>/dev/null && kill -9 "$BPID" 2>/dev/null
  if [ $KEEP = 1 ]; then echo "kept: $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT

#
# -i: a line that fails does not stop the rest - a failed copy of coraine must not hide what the
# copies of the plugins after it do to the running broker. Success is then: no error in the log.
#
install_from()
{
  make -i -s -C "$REPO" install_debug BUILD_DEBUG="$1" PREFIX="$T" PLUGIN_DIR="$T/plugins" ETC_DIR="$T/etc" > "$T/install.$2.log" 2>&1
  ! grep -q 'Error\|cannot\|busy' "$T/install.$2.log"
}


echo "1. install $BUILD into $T"
mkdir -p "$T/bin"
install_from "$BUILD" first || { cat "$T/install.first.log"; exit 1; }
cp -p "$RAMDB" "$T/plugins/db/currentState/ramDB.so"


echo "2. start the broker (-db ramDB, port $PORT)"
COR_TEST_NO_REAP=on SEAMWARE_PLUGIN_DIR="$T/plugins" SEAMWARE_ETC_DIR="$T/etc" \
  "$T/bin/coraine" -db ramDB -p "$PORT" -fg > "$T/broker.log" 2>&1 < /dev/null &
BPID=$!
for i in $(seq 1 50); do curl -s -m 1 -o /dev/null "localhost:$PORT/version" && break; sleep 0.1; done
code=$(curl -s -o /dev/null -w '%{http_code}' "localhost:$PORT/ngsi-ld/v1/entities" -H 'Content-Type: application/json' \
       -d '{"id":"urn:ngsi-ld:T:1","type":"T","p":{"type":"Property","value":1}}')
[ "$code" = 201 ] || { fail "the entity was not created ($code)"; exit $FAILS; }


echo "3. install the next build (plugin contents changed) under the running broker"
N="$T/next"
mkdir -p "$N"
( cd "$BUILD" && find src/app/coraine/coraine src/app/coraineImport/coraine-import src/plugins -type f \( -name coraine -o -name coraine-import -o -name '*.so' \) 2>/dev/null ) | while read -r f; do
  mkdir -p "$N/$(dirname "$f")"
  cp -p "$BUILD/$f" "$N/$f"
done
cp -p "$BUILD/src/plugins/temporal/timescale/timescale.so" "$N/src/plugins/temporal/none/none.so"
cp -p "$BUILD/src/plugins/temporal/none/none.so"           "$N/src/plugins/temporal/timescale/timescale.so"

declare -A inodeBefore
for f in "$T"/bin/coraine "$T"/plugins/*/*.so "$T"/plugins/*/*/*.so; do
  [ -f "$f" ] && inodeBefore[$f]=$(stat -c %i "$f")
done

if install_from "$N" next; then
  ok "make install_debug under the running broker"
else
  fail "make install_debug under the running broker: $(grep -m1 'busy\|Error\|cannot' "$T/install.next.log")"
fi


echo "4. check"
same=0
for f in "${!inodeBefore[@]}"; do
  [ "$f" = "$T/plugins/db/currentState/ramDB.so" ] && continue   # not the install's: copied in step 1
  if [ "$(stat -c %i "$f")" = "${inodeBefore[$f]}" ]; then
    fail "rewritten in place (same inode): ${f#$T/}"
    same=$((same + 1))
  fi
done
[ $same = 0 ] && ok "every installed file is a new file (inode)"

sleep 2
code=$(curl -s -m 3 -o /dev/null -w '%{http_code}' "localhost:$PORT/ngsi-ld/v1/entities/urn:ngsi-ld:T:1")
[ "$code" = 200 ] && ok "the broker answers after the install" || fail "the broker does not answer after the install ($code)"

kill -TERM "$BPID" 2>/dev/null
wait "$BPID"
rc=$?
BPID=
[ $rc = 0 ] && ok "the broker stopped cleanly (exit 0)" || fail "the broker's exit status on SIGTERM: $rc"
if grep -q "coraine crashed" "$T/broker.log"; then
  fail "crash report in the broker's log:"
  sed -n '/coraine crashed/,/end of crash report/p' "$T/broker.log" | head -12
fi

echo "$FAILS failed"
exit $FAILS
