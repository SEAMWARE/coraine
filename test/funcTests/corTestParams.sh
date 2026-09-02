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
              "$(mongosh --host ${COR_MONGO_HOST:-localhost} --port ${COR_MONGO_PORT:-27017} --quiet --eval 'db.adminCommand({isMaster:1}).setName ? "yes" : "no"' 2>/dev/null || echo no)" \
              "HA cache-sync tests: yes|no (yes needs a mongo replica set)" "HA"

#
# -traceLevels: what the brokers this suite starts write to their log.
#
# The logs are only ever COLLECTED on a failure (ci.yml uploads them then), so a
# run that goes green pays the write and nothing reads it - and a run that goes
# red is the one case where "the broker said nothing at all" is the worst
# possible answer. ha_cache_sync failed in CI with a missing notification and
# its artifact contained four EMPTY broker logs; that is what this is for.
#
# Not every level: the forwarded-request/response BODY traces (223, 226) and the
# notification body (233) render a full payload per distributed operation, which
# the distop and valgrind tests pay for by the hundred. Everything else is a
# line or two per decision.
#
corCliParamAdd "-traceLevels" "COR_TRACE_LEVELS" \
              "200-222,224,225,227,230,231,232,234,235,240-255" \
              "Broker trace levels (kargs syntax); empty for none" "TRACELEVELS"
