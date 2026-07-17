CMAKE_MINIMUM_REQUIRED(VERSION 3.5)
PROJECT(swBroker C)

#
# Build type - default to Release
#
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

#
# Feature flags for embedded / minimal-footprint builds.
#
# NOTE: as of the cmake migration the swBroker sources do NOT yet carry the
# corresponding #ifdefs, so these flags currently only gate which source files
# are *compiled in* (per the per-component CMakeLists). They all default ON, so
# a default build is identical to the old makefile build. Turning one OFF will
# drop sources whose symbols are still referenced elsewhere until the matching
# #ifdefs are added to the C code — treat the flags as scaffolding for now.
#
option(SW_FEATURE_SUBSCRIPTIONS   "Enable Context Information Subscriptions + notifications"   ON)
option(SW_FEATURE_REGISTRATIONS   "Enable Context Source Registration CRUD (§ 12)"            ON)
option(SW_FEATURE_GEOQ            "Enable geo-query matching (requires GEOS)"                  ON)
option(SW_FEATURE_SCOPES          "Enable entity scope + scopeQ filtering"                     ON)
option(SW_FEATURE_DATASETID       "Enable multi-instance attributes via datasetId"             ON)
option(SW_FEATURE_MULTI_TYPE      "Enable multi-type entities (type as array)"                 ON)
option(SW_FEATURE_CONTEXT_DL      "Enable @context downloading from URLs (vs built-in only)"   ON)
option(SW_FEATURE_CONTEXT_HOSTING "Enable /jsonldContexts CRUD endpoints + DB persistence"     ON)
option(SW_FEATURE_TENANTS         "Enable multi-tenancy (NGSILD-Tenant header)"                ON)
option(SW_FEATURE_MONGOC          "Build the mongoc DB plugin"                                 ON)
option(SW_FEATURE_ADMIN_API       "Build the admin API plugin"                                 ON)
option(SW_FEATURE_METRICS         "Enable Prometheus metrics via kprom (adds /admin/metrics)"  ON)
option(SW_FEATURE_LOCATION         "Enable geo-aware dispatch on the 'location' GeoProperty"          ON)
option(SW_FEATURE_OBSERVATION_SPACE "Enable geo-aware dispatch on the 'observationSpace' GeoProperty" OFF)
option(SW_FEATURE_OPERATION_SPACE  "Enable geo-aware dispatch on the 'operationSpace' GeoProperty"    OFF)

#
# Emit compile-time defines so C code can #ifdef on them (once added)
#
foreach(feature SUBSCRIPTIONS REGISTRATIONS GEOQ SCOPES DATASETID MULTI_TYPE CONTEXT_DL CONTEXT_HOSTING TENANTS MONGOC ADMIN_API METRICS LOCATION OBSERVATION_SPACE OPERATION_SPACE)
  if (SW_FEATURE_${feature})
    add_compile_definitions(SW_FEATURE_${feature}=1)
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
set(CMAKE_C_FLAGS_COVERAGE "-g -O0 --coverage")

#
# sw-libs / k-libs live in sibling repos (one .a per repo), linked statically.
#
set(SW_LIBS_DIR ${PROJECT_SOURCE_DIR}/..)

include_directories(${SW_LIBS_DIR})
include_directories(${PROJECT_SOURCE_DIR}/src/lib)
include_directories(${PROJECT_SOURCE_DIR}/src/app/swBroker)
include_directories(${PROJECT_SOURCE_DIR}/src/plugins)

set(SW_STATIC_LIBS
  ${SW_LIBS_DIR}/swRest/libswRest.a
  ${SW_LIBS_DIR}/swNgsild/libswNgsild.a
  ${SW_LIBS_DIR}/swJsonld/libswJsonld.a
  ${SW_LIBS_DIR}/swPlugin/libswPlugin.a
  ${SW_LIBS_DIR}/kargs/libkargs.a
  ${SW_LIBS_DIR}/ktrace/libktrace.a
  ${SW_LIBS_DIR}/kprom/libkprom.a
  ${SW_LIBS_DIR}/khash/libkhash.a
  ${SW_LIBS_DIR}/klog/libklog.a
  ${SW_LIBS_DIR}/kalloc/libkalloc.a
  ${SW_LIBS_DIR}/kjson/libkjson.a
  ${SW_LIBS_DIR}/kbase/libkbase.a
)

#
# System libraries
#
set(SW_DYNAMIC_LIBS microhttpd ssl crypto pthread dl m mosquitto)

#
# ICU "root" collation for orderBy string ordering (§ 7.6.2.1). ON by default.
# Must match swNgsild's SW_WITH_ICU build flag (which selects the code path);
# this adds the matching ICU libs to the final broker link. Turn both off
# together to build without the libicu dependency.
#
option(SW_FEATURE_ICU_COLLATION "orderBy string ordering via ICU root collation (needs libicu)" ON)
if (SW_FEATURE_ICU_COLLATION)
  find_package(ICU REQUIRED COMPONENTS i18n uc data)
  list(APPEND SW_DYNAMIC_LIBS ICU::i18n ICU::uc ICU::data)
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
ADD_SUBDIRECTORY(src/lib/troe)
ADD_SUBDIRECTORY(src/lib/plugin)
if (SW_FEATURE_METRICS)
  ADD_SUBDIRECTORY(src/lib/metrics)
endif()
ADD_SUBDIRECTORY(src/lib/serviceRoutines)
ADD_SUBDIRECTORY(src/lib/linkedEntities)
if (SW_FEATURE_REGISTRATIONS)
  ADD_SUBDIRECTORY(src/lib/forwarding)
endif()
ADD_SUBDIRECTORY(src/plugins/currentState/swRamDB)
ADD_SUBDIRECTORY(src/plugins/temporal/none)
ADD_SUBDIRECTORY(src/plugins/temporal/ramdb)
ADD_SUBDIRECTORY(src/plugins/temporal/timescale)

if (SW_FEATURE_MONGOC)
  ADD_SUBDIRECTORY(src/plugins/currentState/mongoc)
endif()

if (SW_FEATURE_ADMIN_API)
  ADD_SUBDIRECTORY(src/plugins/api/admin)
endif()

ADD_SUBDIRECTORY(src/app/swBroker)
ADD_SUBDIRECTORY(test)
