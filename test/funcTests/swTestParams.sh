#
# swTestParams.sh - repo-specific CLI parameters for swBroker test suite
#
# These are registered before argument parsing in swTest, so they
# appear in -u (usage) and are parsed alongside built-in options.
#

swCliParamAdd "-db"     "SW_DB_TYPE"      "NONE" "Current-state DB: ramdb|mongoc"      "DB"
swCliParamAdd "-troeDb" "SW_TROE_DB_TYPE" "NONE" "TRoE DB: postgres|mongo|..."       "TROEDB"
