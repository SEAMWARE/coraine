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
# NOTE: as of the cmake migration the coraine sources do NOT yet carry the
# corresponding #ifdefs, so these flags currently only gate which source files
# are *compiled in* (per the per-component CMakeLists). They all default ON, so
# a default build is identical to the old makefile build. Turning one OFF will
# drop sources whose symbols are still referenced elsewhere until the matching
# #ifdefs are added to the C code — treat the flags as scaffolding for now.
#
option(COR_FEATURE_SUBSCRIPTIONS   "Enable Context Information Subscriptions + notifications"   ON)
option(COR_FEATURE_REGISTRATIONS   "Enable Context Source Registration CRUD (§ 12)"            ON)
option(COR_FEATURE_GEOQ            "Enable geo-query matching (requires GEOS)"                  ON)
option(COR_FEATURE_SCOPES          "Enable entity scope + scopeQ filtering"                     ON)
option(COR_FEATURE_DATASETID       "Enable multi-instance attributes via datasetId"             ON)
option(COR_FEATURE_MULTI_TYPE      "Enable multi-type entities (type as array)"                 ON)
option(COR_FEATURE_CONTEXT_DL      "Enable @context downloading from URLs (vs built-in only)"   ON)
option(COR_FEATURE_CONTEXT_HOSTING "Enable /jsonldContexts CRUD endpoints + DB persistence"     ON)
option(COR_FEATURE_TENANTS         "Enable multi-tenancy (NGSILD-Tenant header)"                ON)
option(COR_FEATURE_MONGOC          "Build the mongoc DB plugin"                                 ON)
option(COR_FEATURE_ADMIN_API       "Build the admin API plugin"                                 ON)
option(COR_FEATURE_METRICS         "Enable Prometheus metrics via kprom (adds /admin/metrics)"  ON)
option(COR_FEATURE_LOCATION         "Enable geo-aware dispatch on the 'location' GeoProperty"          ON)
option(COR_FEATURE_OBSERVATION_SPACE "Enable geo-aware dispatch on the 'observationSpace' GeoProperty" OFF)
option(COR_FEATURE_OPERATION_SPACE  "Enable geo-aware dispatch on the 'operationSpace' GeoProperty"    OFF)

#
# Emit compile-time defines so C code can #ifdef on them (once added)
#
foreach(feature SUBSCRIPTIONS REGISTRATIONS GEOQ SCOPES DATASETID MULTI_TYPE CONTEXT_DL CONTEXT_HOSTING TENANTS MONGOC ADMIN_API METRICS LOCATION OBSERVATION_SPACE OPERATION_SPACE)
  if (COR_FEATURE_${feature})
    add_compile_definitions(COR_FEATURE_${feature}=1)
    message(STATUS "Feature ${feature}: ON")
  else()
    message(STATUS "Feature ${feature}: OFF")
  endif()
endforeach()

#
# Compiler flags
#
set(CMAKE_C_FLAGS         "-Wall -Werror -fPIC -DLOG_ON -Wno-unused-function")
set(CMAKE_C_FLAGS_RELEASE  "-O2 -g")
set(CMAKE_C_FLAGS_DEBUG    "-g -DDEBUG")
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
set(CMAKE_C_FLAGS_COVERAGE "-g -O0 --coverage -fprofile-update=atomic")

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

set(COR_STATIC_LIBS
  ${COR_LIBS_DIR}/corRest/libcorRest.a
  ${COR_LIBS_DIR}/corNgsild/libcorNgsild.a
  ${COR_LIBS_DIR}/corJsonld/libcorJsonld.a
  ${COR_LIBS_DIR}/corPlugin/libcorPlugin.a
  ${COR_LIBS_DIR}/kargs/libkargs.a
  ${COR_LIBS_DIR}/ktrace/libktrace.a
  ${COR_LIBS_DIR}/kprom/libkprom.a
  ${COR_LIBS_DIR}/khash/libkhash.a
  ${COR_LIBS_DIR}/kalloc/libkalloc.a
  ${COR_LIBS_DIR}/kjson/libkjson.a
  ${COR_LIBS_DIR}/kbase/libkbase.a
)

#
# System libraries
#
set(COR_DYNAMIC_LIBS microhttpd ssl crypto pthread dl m mosquitto)

#
# ICU "root" collation for orderBy string ordering (§ 7.6.2.1). ON by default.
# Must match corNgsild's COR_WITH_ICU build flag (which selects the code path);
# this adds the matching ICU libs to the final broker link. Turn both off
# together to build without the libicu dependency.
#
option(COR_FEATURE_ICU_COLLATION "orderBy string ordering via ICU root collation (needs libicu)" ON)
if (COR_FEATURE_ICU_COLLATION)
  find_package(ICU REQUIRED COMPONENTS i18n uc data)
  list(APPEND COR_DYNAMIC_LIBS ICU::i18n ICU::uc ICU::data)
  message(STATUS "Feature ICU_COLLATION: ON (ICU ${ICU_VERSION})")
else()
  message(STATUS "Feature ICU_COLLATION: OFF")
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
if (COR_FEATURE_METRICS)
  ADD_SUBDIRECTORY(src/lib/metrics)
endif()
ADD_SUBDIRECTORY(src/lib/serviceRoutines)
ADD_SUBDIRECTORY(src/lib/linkedEntities)
if (COR_FEATURE_REGISTRATIONS)
  ADD_SUBDIRECTORY(src/lib/forwarding)
endif()
ADD_SUBDIRECTORY(src/plugins/currentState/corDB)
ADD_SUBDIRECTORY(src/plugins/temporal/none)
ADD_SUBDIRECTORY(src/plugins/temporal/ramdb)
ADD_SUBDIRECTORY(src/plugins/temporal/timescale)

if (COR_FEATURE_MONGOC)
  ADD_SUBDIRECTORY(src/plugins/currentState/mongoc)
endif()

if (COR_FEATURE_ADMIN_API)
  ADD_SUBDIRECTORY(src/plugins/api/admin)
endif()

ADD_SUBDIRECTORY(src/app/coraine)
ADD_SUBDIRECTORY(test)
