# SPDX-License-Identifier: Apache-2.0
#
# corTestParams.sh - repo-specific CLI parameters for coraine test suite
#
# These are registered before argument parsing in corTest, so they
# appear in -u (usage) and are parsed alongside built-in options.
#

corCliParamAdd "-db"     "COR_DB_TYPE"      "mongoc" "Current-state DB: corDB|mongoc"    "DB"
corCliParamAdd "-troeDb" "COR_TROE_DB_TYPE" "NONE" "TRoE DB: postgres|mongo|..."       "TROEDB"

#
# -ha: does this environment support the HA cache sync (--ha mongo)?
#
# It rides on a mongo CHANGE STREAM, which reads the oplog - so it needs a
# replica set, and a standalone mongod cannot run those tests at all. That is a
# property of the environment and not a choice, so it is DETECTED rather than
# defaulted: on a replica set the HA tests run without anyone having to remember
# a flag, and on a standalone they simply do not apply - they leave the run set
# rather than being reported as skipped, which they never were. Override with
# -ha yes|no.
#
corCliParamAdd "-ha" "COR_HA" \
              "$(mongosh --port ${COR_MONGO_PORT:-27017} --quiet --eval 'db.adminCommand({isMaster:1}).setName ? "yes" : "no"' 2>/dev/null || echo no)" \
              "HA cache-sync tests: yes|no (yes needs a mongo replica set)" "HA"
