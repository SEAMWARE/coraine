# SPDX-License-Identifier: Apache-2.0
SET(SOURCES
  corDbGlobals.c
  corDbStore.c
  corDbRegister.c
  corDbInit.c
  corDbClose.c
  corDbEntityCreate.c
  corDbEntityBulkCreate.c
  corDbEntityBulkUpdate.c
  corDbEntityBulkMerge.c
  corDbEntityBulkDelete.c
  corDbEntityRetrieve.c
  corDbEntityQuery.c
  corDbEntityDelete.c
  corDbEntityMerge.c
  corDbEntityReplace.c
  corDbEntityAttrsSet.c
  corDbTypeList.c
  corDbAttrList.c
  corDbGeoMatch.c
  ${CMAKE_SOURCE_DIR}/src/plugins/shared/geoMatch.c
)

IF (COR_FEATURE_SUBSCRIPTIONS)
  LIST(APPEND SOURCES
    corDbSubscriptionCreate.c
    corDbSubscriptionRetrieve.c
    corDbSubscriptionQuery.c
    corDbSubscriptionUpdate.c
    corDbSubscriptionReplace.c
    corDbSubscriptionDelete.c
  )
ENDIF()

IF (COR_FEATURE_REGISTRATIONS)
  LIST(APPEND SOURCES
    corDbRegistrationCreate.c
    corDbRegistrationRetrieve.c
    corDbRegistrationQuery.c
    corDbRegistrationUpdate.c
    corDbRegistrationDelete.c
  )
ENDIF()

ADD_LIBRARY(corDB SHARED ${SOURCES})

TARGET_COMPILE_DEFINITIONS(corDB PRIVATE PLUGIN_VERSION="0.2.0")
# Plugin .so resolves NGSI-LD/k-lib symbols from the (rdynamic) broker at dlopen.
TARGET_LINK_LIBRARIES(corDB geos_c m)

SET_TARGET_PROPERTIES(corDB PROPERTIES OUTPUT_NAME "corDB" PREFIX "")
