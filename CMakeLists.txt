# SPDX-License-Identifier: Apache-2.0
CMAKE_MINIMUM_REQUIRED(VERSION 3.5)
PROJECT(coraine C)

#
# Build type - default to Release
#
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

#
# Feature flags for embedded / minimal-footprint builds.
#
# The feature switches, in groups. "# Group: <name> - <what>" heads each group, and every option() below
# one is in it: coraine-build --list-features --json reports each switch's group (c3 shows them so). A new
# switch goes into the group it belongs to.
#
# Group: NGSI-LD API - what the API offers beyond entities and their attributes
option(COR_FEATURE_SUBSCRIPTIONS   "Enable Context Information Subscriptions + notifications"   ON)
option(COR_FEATURE_REGISTRATIONS   "Enable Context Source Registration CRUD (§ 12)"            ON)
option(COR_FEATURE_AUTO_ENTITY_MAP "Enable automatic EntityMaps: a query of more than one page - distributed by default, local too with --autoEntityMaps all - freezes its set of matches and serves its pages from it (--entityMapMemory)" ON)
option(COR_FEATURE_RESPONSE_BUDGET "Enable the byte budget of an entity query (--maxResponseSize): a shorter page, or 403 TooManyResults" ON)
option(COR_FEATURE_TIMEPOINT       "Enable the timepoint resources: an entity, a query, as it was at timeAt (/temporal/timepoint/..., from the TRoE's history)" ON)
#
# OFF, alone among these. ON links libicu: three shared libraries and 39.2 MiB,
# of which libicudata is 31.6 MiB - nine times the size of the whole broker, for
# a collation table. Every other feature here costs kilobytes of our own .text.
# Must match corNgsild's COR_WITH_ICU (which selects the code path); the makefile
# drives both from one variable so they cannot drift.
#
option(COR_FEATURE_ICU_COLLATION   "orderBy string ordering via ICU root collation (needs libicu)" OFF)

# Group: Snapshots - writes and subscriptions on a Snapshot (§ 7.9.2)
option(COR_FEATURE_SNAPSHOT_WRITE "Enable writes on a Snapshot (NGSILD-Snapshot on an entity / temporal write, local scope - § 7.9.2)" ON)
option(COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS "Enable subscriptions on a Snapshot (NGSILD-Snapshot on /subscriptions, notified by writes on the snapshot - § 7.9.2)" ON)

# Group: JSON-LD @context - where the broker's @contexts come from
option(COR_FEATURE_CONTEXT_HOSTING "Enable /jsonldContexts CRUD endpoints + DB persistence"     ON)

# Group: Geo - geo-queries
option(COR_FEATURE_GEOQ            "Enable geo-query matching (requires GEOS)"                  ON)

# Group: Security - who sent a request, whether it may, and the policies that decide
option(COR_FEATURE_AUTH            "Enable auth plugins - who sent a request and whether it may (--auth; fdsc.so: the FIWARE Data Space Connector's tokens and policies, doc/authorization.md)" ON)
option(COR_FEATURE_AUTH_OIDC       "Enable oidc.so - an OpenID provider's access tokens (Keycloak, ...), their roles reading or writing (--auth oidc, doc/authorization.md)" ON)
option(COR_FEATURE_POLICY_API    "Enable the ODRL-PAP's /policy API on a port of its own (--policyApiPort): the policies fdsc.so reads, held by the broker (doc/authorization.md)" ON)

# Group: Beyond NGSI-LD - Service Execution, and what a bridge plugin can do in the broker
option(COR_FEATURE_SERVICE_EXECUTION "Enable Service Execution - service registrations, executions, executors (ETSI GR CIM-055)" ON)
option(COR_FEATURE_DATA_CHANNELS "Enable DataChannels - a bridge endpoint whose samples are records (a CKAN row), each made into entities by its mapping (\"dataChannels\" in --bridgeConfig, GET /ngsi-ld/v1/dataChannels, doc/bridge-channels.md)" ON)
option(COR_FEATURE_CHANNEL_STATUS_IN "Enable BridgeBroker.channelStatusIn (bridge ABI 12) - a bridge plugin reports whether a Channel carries, and why not, as the Channel's status and statusReason" ON)

# Group: Protocols - the API over something else than HTTP
#
# COR_PROTOCOL must match corRest's COR_WITH_COR_PROTOCOL (which leaves corRestCor.c out of the lib); the
# makefile drives both from that one variable, as it does ICU_COLLATION. The codec stays either way:
# corDB's log is written in it.
#
option(COR_FEATURE_COR_PROTOCOL    "Enable cor:// - the binary NGSI-LD protocol: served on --corPort, and forwarding to a registration whose endpoint is cor://" ON)
option(COR_FEATURE_TRANSPORTS      "Enable transport plugins - the API over another protocol (--transports; ws.so)" ON)

# Group: Operations - running the broker: its admin API, metrics and health probes
option(COR_FEATURE_ADMIN_API       "Build the admin API plugin"                                 ON)
option(COR_FEATURE_METRICS         "Enable Prometheus metrics via corProm (adds /admin/metrics)"  ON)
option(COR_FEATURE_HEALTH        "Enable the health port (--healthPort): /live, /ready and a report, on a thread of its own" ON)

# Group: Stores and tools - the plugins and programs built beside the broker
option(COR_FEATURE_MONGOC          "Build the mongoc DB plugin"                                 ON)
option(COR_FEATURE_MIGRATE           "Build coraine-import - a migration stream (from another broker's database) written into the stores (doc/migration.md)" ON)

#
# Emit a compile-time define for EVERY feature - 1 when on, 0 when off, never
# absent. So the source says `#if COR_FEATURE_X`, not `#ifdef`, and combined
# with -Wundef (below) a misspelt feature name is a compile error instead of a
# silently-false test that quietly compiles the code out.
#
# The same 0/1 also makes a feature usable as a plain C value - which is how
# coraineFeatures.c reports the build's feature set on --version and /version
# without fifteen #ifdefs.
#
set(COR_FEATURES SUBSCRIPTIONS REGISTRATIONS GEOQ CONTEXT_HOSTING MONGOC ADMIN_API METRICS AUTH POLICY_API COR_PROTOCOL TRANSPORTS SERVICE_EXECUTION SNAPSHOT_WRITE SNAPSHOT_SUBSCRIPTIONS MIGRATE HEALTH RESPONSE_BUDGET AUTO_ENTITY_MAP CHANNEL_STATUS_IN DATA_CHANNELS ICU_COLLATION TIMEPOINT AUTH_OIDC)
#
# A subscription on a Snapshot is a subscription (SUBSCRIPTIONS), notified by the writes on the
# snapshot (SNAPSHOT_WRITE) - without either it is off, and reported off.
#
if (NOT COR_FEATURE_SUBSCRIPTIONS OR NOT COR_FEATURE_SNAPSHOT_WRITE)
  set(COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS OFF)
endif()

#
# The metrics are served by the admin API plugin (GET /admin/metrics, GET /metrics) and by nothing else:
# without it, nothing could read them.
#
if (NOT COR_FEATURE_ADMIN_API)
  set(COR_FEATURE_METRICS OFF)
endif()

#
# oidc.so is an auth plugin: without the auth plugins, nothing would load it.
#
if (NOT COR_FEATURE_AUTH)
  set(COR_FEATURE_AUTH_OIDC OFF)
endif()

#
# The policy API listens on a port of its own, and only libmicrohttpd has a second listener (corRestListen):
# with the built-in HTTP server it is off. COR_HTTP_SERVER is read here before its block below gives it its
# default - unset is mhd.
#
if (DEFINED COR_HTTP_SERVER AND NOT COR_HTTP_SERVER STREQUAL "mhd")
  set(COR_FEATURE_POLICY_API OFF)
endif()

foreach(feature ${COR_FEATURES})
  if (COR_FEATURE_${feature})
    add_compile_definitions(COR_FEATURE_${feature}=1)
    message(STATUS "Feature ${feature}: ON")
  else()
    add_compile_definitions(COR_FEATURE_${feature}=0)
    message(STATUS "Feature ${feature}: OFF")
  endif()
endforeach()

#
# What cmake knows about this build that the makefile cannot: the flavour and the
# compiler it actually chose. GET /build reports both. The git sha and the build
# timestamp come the other way, from the makefile, in coraineBuild.h - each fact
# from whoever knows it rather than one of them guessing.
#
add_compile_definitions(CORAINE_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
add_compile_definitions(CORAINE_BUILD_COMPILER="${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")

#
# Compiler flags
#
set(CMAKE_C_FLAGS         "-Wall -Werror -Wundef -fPIC -DLOG_ON")
#
# A release build's flags - COR_C_FLAGS_RELEASE where the caller gives them (`make pgo` adds the
# profile flags there). Not CMAKE_C_FLAGS_RELEASE: the set() below shadows whatever -D gives that
# one, and `make pgo` had been passing its profile flags there - linked with -fprofile-generate, the
# broker's own objects were compiled without it, so neither instrumented nor profile-guided.
#
if (NOT DEFINED COR_C_FLAGS_RELEASE)
  set(COR_C_FLAGS_RELEASE "-O2 -g")
endif()
set(CMAKE_C_FLAGS_RELEASE  "${COR_C_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_DEBUG    "-g -DDEBUG -DCOR_T_ON")
#
# COR_T_ON: traces (COR_T, and the libraries' COR_LIB_T) are compiled in - Debug and
# Coverage. A Release build has none: they are compiled away (corLog.h), and a trace
# level asked for there has nothing to turn on (coraine.c says so at start).
#
#
# -fprofile-update=atomic, because this broker is thread-per-connection.
#
# gcov's counters are ordinary non-atomic increments by default. Two connections
# executing the same line at the same time then race on one counter: increments are
# LOST, and a counter can end up NEGATIVE, which is not a curiosity - gcovr treats a
# negative count as fatal, and the nightly of 2026-08-25 spent fifty minutes and
# produced no report because of one negative BRANCH count.
#
# The pinned gcovr (>= 8.3, see corLibs/docker/Dockerfile.ci-nightly) survives that
# with --gcov-ignore-parse-errors, but surviving is not the same as being right: the
# lost increments are silent, and they only ever bias coverage DOWNWARDS. Atomic
# updates make the numbers correct, which is the point of measuring them.
#
# It is not free - an atomic add costs more than an add - but it is confined to this
# build type. Release and Debug never see it, and the nightly's performance job
# measures a release build, so no recorded figure can move.
#
set(CMAKE_C_FLAGS_COVERAGE "-g -O0 --coverage -fprofile-update=atomic -DCOR_T_ON")

#
# Cor-Libs / k-libs live in sibling repos (one .a per repo), linked statically.
#
#
# The version lives in ONE place: src/app/coraine/coraineVersion.h, which is
# what the broker itself compiles in. Everything that needs to report a version
# reads it from here. Hand-kept copies drift - this project had four, and the
# broker was announcing 0.3 in its User-Agent while /admin/version said 0.2.0.
#
FILE(STRINGS ${PROJECT_SOURCE_DIR}/src/app/coraine/coraineVersion.h _cor_version_line
     REGEX "^#define[ \t]+CORAINE_VERSION[ \t]+\"")
STRING(REGEX REPLACE "^#define[ \t]+CORAINE_VERSION[ \t]+\"([^\"]+)\".*" "\\1"
       CORAINE_VERSION "${_cor_version_line}")
IF(NOT CORAINE_VERSION)
  MESSAGE(FATAL_ERROR "could not read CORAINE_VERSION from src/app/coraine/coraineVersion.h")
ENDIF()
MESSAGE(STATUS "coraine version: ${CORAINE_VERSION}")

set(COR_LIBS_DIR ${PROJECT_SOURCE_DIR}/..)

include_directories(${COR_LIBS_DIR})
include_directories(${PROJECT_SOURCE_DIR}/src/lib)
include_directories(${PROJECT_SOURCE_DIR}/src/app/coraine)
include_directories(${PROJECT_SOURCE_DIR}/src/plugins)

#
# The DB / TRoE plugin interface stamp (tools/dbAbiStamp.sh; doc/plugin-architecture.md, "The DB plugin
# interface stamp"): a hash of the headers the broker and its DB / TRoE plugins share structs through.
# Generated into the build tree on EVERY build - a header edit needs no reconfigure - and rewritten only
# when it changed, so only what includes it recompiles. The broker's pluginLoader.c (coraineDbAbi) and
# every DB / TRoE plugin's shared/dbPluginAbi.c (dbPluginAbi) include it; their targets depend on this one.
#
set(COR_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
include_directories(${COR_GENERATED_DIR})
add_custom_target(dbAbiStamp
  COMMAND ${CMAKE_COMMAND} -E env COR_LIBS=${COR_LIBS_DIR} ${PROJECT_SOURCE_DIR}/tools/dbAbiStamp.sh --header ${COR_GENERATED_DIR}/dbAbiStamp.h
  BYPRODUCTS ${COR_GENERATED_DIR}/dbAbiStamp.h
  VERBATIM
)

set(COR_STATIC_LIBS
  ${COR_LIBS_DIR}/corRest/libcorRest.a
  ${COR_LIBS_DIR}/corNgsild/libcorNgsild.a
  ${COR_LIBS_DIR}/corJsonld/libcorJsonld.a
  ${COR_LIBS_DIR}/corPlugin/libcorPlugin.a
  ${COR_LIBS_DIR}/corBridge/libcorBridge.a
  ${COR_LIBS_DIR}/corProm/libcorProm.a
  ${COR_LIBS_DIR}/corJson/libcorJson.a
  ${COR_LIBS_DIR}/corTree/libcorTree.a
  ${COR_LIBS_DIR}/corArgs/libcorArgs.a
  ${COR_LIBS_DIR}/corLog/libcorLog.a
  ${COR_LIBS_DIR}/corHash/libcorHash.a
  ${COR_LIBS_DIR}/corAlloc/libcorAlloc.a
  ${COR_LIBS_DIR}/corBase/libcorBase.a
)

#
# System libraries
#
#
# COR_HTTP_SERVER - which HTTP server this build carries. Two, and exactly one.
#
#   mhd      libmicrohttpd, an external shared library built from source in the
#            Dockerfile and mapped by every broker process
#   builtin  the epoll server in corRest, no external dependency
#
# NOT a COR_FEATURE_* boolean: those answer "is this capability in the build",
# and this is a choice between two implementations of a capability that is
# always in it. A boolean would have to be read as "MHD = off means... what?"
# and would have nowhere to go if a third implementation ever arrives.
#
# It still emits a 0/1 PAIR rather than a name, so the source reads
# `#if COR_HTTP_SERVER_MHD` and -Wundef catches a misspelling - the same
# discipline as the feature flags.
#
set(COR_HTTP_SERVER "mhd" CACHE STRING "HTTP server implementation: mhd | builtin")
set_property(CACHE COR_HTTP_SERVER PROPERTY STRINGS mhd builtin)

if (COR_HTTP_SERVER STREQUAL "mhd")
  add_compile_definitions(COR_HTTP_SERVER_MHD=1 COR_HTTP_SERVER_BUILTIN=0)
elseif (COR_HTTP_SERVER STREQUAL "builtin")
  add_compile_definitions(COR_HTTP_SERVER_MHD=0 COR_HTTP_SERVER_BUILTIN=1)

  #
  # corHttp is a static archive of its own and NOT folded into libcorRest.a - an
  # archive cannot contain another one - so the built-in flavour links both.
  # Appended AFTER corRest, because corRest is what calls into it and the linker
  # resolves an archive against what is still undefined when it reaches it.
  #
  list(APPEND COR_STATIC_LIBS ${COR_LIBS_DIR}/corHttp/libcorHttp.a)
else()
  message(FATAL_ERROR "COR_HTTP_SERVER must be 'mhd' or 'builtin', not '${COR_HTTP_SERVER}'")
endif()
message(STATUS "HTTP server: ${COR_HTTP_SERVER}")

# Reported by GET /build, so a running broker can say which one it is.
add_compile_definitions(CORAINE_HTTP_SERVER="${COR_HTTP_SERVER}")

set(COR_DYNAMIC_LIBS ssl crypto pthread dl m)

# The one library the builtin server exists to remove. Linked only when it is
# the implementation in use - which is what makes `ldd coraine` the check that
# the switch actually worked.
if (COR_HTTP_SERVER STREQUAL "mhd")
  list(APPEND COR_DYNAMIC_LIBS microhttpd)
endif()

#
# The link half of COR_FEATURE_ICU_COLLATION, declared with the other features
# above (it has to be, to reach the COR_FEATURES loop that emits its 0/1 and
# reports it on --version). This adds the ICU libs to the final broker link when
# it is on; corNgsild's COR_WITH_ICU selects the code path that calls them.
#
if (COR_FEATURE_ICU_COLLATION)
  find_package(ICU REQUIRED COMPONENTS i18n uc data)
  list(APPEND COR_DYNAMIC_LIBS ICU::i18n ICU::uc ICU::data)
  message(STATUS "  ICU_COLLATION is ON - linking ICU ${ICU_VERSION}")
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Coverage")
  set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} --coverage")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
endif()

#
# Subdirectories
#
ADD_SUBDIRECTORY(src/lib/db)
ADD_SUBDIRECTORY(src/lib/ha)
ADD_SUBDIRECTORY(src/lib/troe)
ADD_SUBDIRECTORY(src/lib/plugin)
ADD_SUBDIRECTORY(src/lib/startup)
ADD_SUBDIRECTORY(src/lib/bridge)
if (COR_FEATURE_TRANSPORTS)
  ADD_SUBDIRECTORY(src/lib/transport)
endif()
if (COR_FEATURE_AUTH)
  ADD_SUBDIRECTORY(src/lib/auth)
endif()
if (COR_FEATURE_POLICY_API)
  ADD_SUBDIRECTORY(src/lib/policyApi)
endif()
if (COR_FEATURE_METRICS)
  ADD_SUBDIRECTORY(src/lib/metrics)
endif()
if (COR_FEATURE_SERVICE_EXECUTION)
  ADD_SUBDIRECTORY(src/lib/serviceExecution)
endif()
ADD_SUBDIRECTORY(src/lib/serviceRoutines)
if (COR_FEATURE_MIGRATE)
  ADD_SUBDIRECTORY(src/lib/migrate)
endif()
ADD_SUBDIRECTORY(src/lib/linkedEntities)
if (COR_FEATURE_REGISTRATIONS)
  ADD_SUBDIRECTORY(src/lib/forwarding)
endif()
ADD_SUBDIRECTORY(src/plugins/temporal/none)
ADD_SUBDIRECTORY(src/plugins/temporal/timescale)

if (COR_FEATURE_MONGOC)
  ADD_SUBDIRECTORY(src/plugins/currentState/mongoc)
endif()

if (COR_FEATURE_ADMIN_API)
  ADD_SUBDIRECTORY(src/plugins/api/admin)
endif()

#
# The loopback bridge. Not a transport - it hands back what it is given, from a
# thread of its own, which is the one property of a real bridge the broker has
# to survive. It is what lets the bridge seam be tested with no transport
# library present, and it is the reference a new bridge is written against.
#
ADD_SUBDIRECTORY(src/plugins/bridge/loopback)

#
# ws - WebSocket as a transport of the broker's API (--transports ws, doc/websocket.md)
#
if (COR_FEATURE_TRANSPORTS)
  ADD_SUBDIRECTORY(src/plugins/transport/ws)
endif()

#
# The auth plugins: jwt - what they share (an access token checked); fdsc - the FIWARE Data Space Connector's
# security inside the broker (--auth fdsc); oidc - an OpenID provider's tokens and roles (--auth oidc).
# doc/authorization.md
#
if (COR_FEATURE_AUTH)
  ADD_SUBDIRECTORY(src/plugins/auth/jwt)
  ADD_SUBDIRECTORY(src/plugins/auth/fdsc)
endif()
if (COR_FEATURE_AUTH_OIDC)
  ADD_SUBDIRECTORY(src/plugins/auth/oidc)
endif()

ADD_SUBDIRECTORY(src/app/coraine)
if (COR_FEATURE_MIGRATE)
  ADD_SUBDIRECTORY(src/app/coraineImport)
endif()

#
# The test tools - corTestClient (was ftClient) and corRequest - are corTools' (a sibling repo), built
# by corLibs and installed into corLibs/bin, beside corTest.
#
