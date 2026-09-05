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


#
# COR_TEST_FEATURES - the optional capabilities compiled INTO the broker binary
#
# Detected, like -ha above, and for the same reason: it is a property of what is
# installed rather than a choice anyone should have to remember on a command
# line. A build with a feature compiled out is indistinguishable from the
# outside until asked, so the binary is asked - `coraine --version` prints one
#
#   features: SUBSCRIPTIONS=1 REGISTRATIONS=0 ...
#
# line, and the names that are ON become the set that a test file's
# REQUIRE_FEATURE: / SKIP_FEATURE: markers are matched against (corTest).
#
# awk rather than grep on purpose - this file is sourced into whatever shell the
# operator is running, and `grep` there may be a wrapper.
#
# Left UNSET if the broker is not on PATH or is too old to print the line. That
# is corTest's feature-blind mode: both markers go inert and every test runs, so
# a broken detection fails loudly instead of quietly skipping the suite.
#
COR_TEST_FEATURES=$("${COR_BROKER:-coraine}" --version 2>/dev/null | awk '/^features: /{ for (i = 2; i <= NF; i++) if ($i ~ /=1$/) { sub(/=1$/, "", $i); printf "%s%s", (n++ ? " " : ""), $i } }')
if [ -n "$COR_TEST_FEATURES" ]; then
  export COR_TEST_FEATURES
else
  unset COR_TEST_FEATURES
fi
