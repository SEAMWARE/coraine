#!/bin/sh
#
# FILE            mongo-rs-init.sh
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# Initiate the single-node replica set that the HA tests need, and wait until it
# is PRIMARY.
#
# WHY A REPLICA SET AT ALL: the HA cache sync (--high-availability mongo) rides on
# a mongo CHANGE STREAM, and a change stream reads the oplog - which a standalone
# mongod does not have. corTestParams.sh probes isMaster.setName and, on a
# standalone, ha_cache_sync.test simply leaves the run set. SILENTLY: nothing in
# the log says the HA paths went unexercised, which is how CI came to report 108
# lines and 10 functions of the broker as untested code rather than as an
# environment nobody had stood up. See doc/coverage.md.
#
# WHY IT IS NOT IN THE IMAGE: quay.io/seamware/mongo-rs starts mongod with
# --replSet, and that is all it can do. A member is addressed by the name its
# CLIENTS use, and the image cannot know that name - here it is the service
# alias, on a workstation it is localhost. So the set is initiated from the
# consumer side, which is this.
#
# WHY IT IS A SCRIPT: three jobs need it - ci.yml's functest matrix and the
# nightly's coverage and valgrind jobs - and three copies of a retry loop that
# must agree is three copies that will not.
#
# Idempotent on purpose. A re-run against a service container that is already
# initiated must be a no-op, not an AlreadyInitialized failure.
#
set -eu

host=${COR_MONGO_HOST:-mongo}
port=${COR_MONGO_PORT:-27017}

mongo() { mongosh --host "$host" --port "$port" --quiet --eval "$1"; }

#
# mongod first, then the set. Nothing else in these jobs waits for mongo - the
# build is long enough that it has always been up by the time the suite starts -
# so this is where the wait lives now.
#
i=0
while [ "$i" -lt 60 ]; do
  mongo 'db.runCommand({ping: 1}).ok' >/dev/null 2>&1 && break
  i=$((i + 1))
  sleep 2
done
[ "$i" -lt 60 ] || { echo "mongod at $host:$port never answered"; exit 1; }

if [ "$(mongo 'try { rs.status().set } catch (e) { "" }')" = "" ]; then
  echo "initiating replica set rs0 with a single member at $host:$port"
  mongo "rs.initiate({_id: 'rs0', members: [{_id: 0, host: '$host:$port'}]})" >/dev/null
else
  echo "replica set already initiated"
fi

#
# Initiated is not the same as usable: an election takes a moment, and a write
# before it lands gets NotWritablePrimary. The suite would see that as a broker
# bug.
#
i=0
while [ "$i" -lt 60 ]; do
  [ "$(mongo 'db.adminCommand({isMaster: 1}).ismaster')" = "true" ] && break
  i=$((i + 1))
  sleep 1
done
[ "$i" -lt 60 ] || { echo "the set never elected a primary"; exit 1; }

#
# The assertion that matters, because it is the exact probe the harness makes:
# a setName is what puts the HA tests in the run set.
#
name=$(mongo 'db.adminCommand({isMaster: 1}).setName')
[ "$name" = "rs0" ] || { echo "setName is '$name', expected rs0"; exit 1; }
echo "replica set rs0 is PRIMARY - the HA tests are in the run set"
