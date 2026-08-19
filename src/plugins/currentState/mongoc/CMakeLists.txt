# SPDX-License-Identifier: Apache-2.0
FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(MONGOC2 REQUIRED mongoc2)

SET(SOURCES
  mongocGlobals.c
  mongocRegister.c
  mongocInit.c
  mongocClose.c
  mongocEntityCreate.c
  mongocEntityBulkCreate.c
  mongocEntityBulkUpdate.c
  mongocEntityBulkMerge.c
  mongocEntityBulkDelete.c
  mongocEntityRetrieve.c
  mongocEntityQuery.c
  mongocEntityDelete.c
  mongocEntityMerge.c
  mongocEntityReplace.c
  mongocEntityAttrsSet.c
  mongocTypeList.c
  mongocAttrList.c
  mongocGeoIndex.c
  mongocTenantSetup.c
  mongocHaWatch.c
  mongocVersion.c
  mongocKjTreeToBson.c
  mongocBsonToKjTree.c
  mongocDotEscape.c
  mongocTenantDrop.c
  mongocInjectType.c
  mongocSnapshotCreate.c
  mongocSnapshotQuery.c
  mongocSnapshotUpdate.c
  mongocSnapshotDelete.c
  ${CMAKE_SOURCE_DIR}/src/plugins/shared/geoMatch.c
)

IF (COR_FEATURE_CONTEXT_HOSTING)
  LIST(APPEND SOURCES mongocContext.c)
ENDIF()

IF (COR_FEATURE_SUBSCRIPTIONS)
  LIST(APPEND SOURCES
    mongocSubscriptionCreate.c
    mongocSubscriptionRetrieve.c
    mongocSubscriptionQuery.c
    mongocSubscriptionUpdate.c
    mongocSubscriptionReplace.c
    mongocSubscriptionDelete.c
    mongocSubscriptionStatsFlush.c
  )
ENDIF()

IF (COR_FEATURE_REGISTRATIONS)
  LIST(APPEND SOURCES
    mongocRegistrationCreate.c
    mongocRegistrationRetrieve.c
    mongocRegistrationQuery.c
    mongocRegistrationUpdate.c
    mongocRegistrationDelete.c
  )
ENDIF()

ADD_LIBRARY(mongoc SHARED ${SOURCES})

TARGET_COMPILE_DEFINITIONS(mongoc PRIVATE MONGOC_PLUGIN_VERSION="0.1.0")
TARGET_INCLUDE_DIRECTORIES(mongoc PRIVATE ${MONGOC2_INCLUDE_DIRS})
# Plugin .so resolves NGSI-LD/k-lib symbols from the (rdynamic) broker at dlopen.
TARGET_LINK_LIBRARIES(mongoc ${MONGOC2_LIBRARIES} geos_c m)

SET_TARGET_PROPERTIES(mongoc PROPERTIES OUTPUT_NAME "mongoc" PREFIX "")
