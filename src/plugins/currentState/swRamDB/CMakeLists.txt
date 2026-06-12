SET(SOURCES
  ramdbGlobals.c
  ramdbStore.c
  ramdbRegister.c
  ramdbInit.c
  ramdbClose.c
  ramdbEntityCreate.c
  ramdbEntityBulkCreate.c
  ramdbEntityBulkUpdate.c
  ramdbEntityBulkMerge.c
  ramdbEntityBulkDelete.c
  ramdbEntityRetrieve.c
  ramdbEntityQuery.c
  ramdbEntityDelete.c
  ramdbEntityMerge.c
  ramdbEntityReplace.c
  ramdbEntityAttrsSet.c
  ramdbTypeList.c
  ramdbAttrList.c
  ramdbGeoMatch.c
  ${CMAKE_SOURCE_DIR}/src/plugins/shared/geoMatch.c
)

IF (SW_FEATURE_SUBSCRIPTIONS)
  LIST(APPEND SOURCES
    ramdbSubscriptionCreate.c
    ramdbSubscriptionRetrieve.c
    ramdbSubscriptionQuery.c
    ramdbSubscriptionUpdate.c
    ramdbSubscriptionReplace.c
    ramdbSubscriptionDelete.c
  )
ENDIF()

IF (SW_FEATURE_REGISTRATIONS)
  LIST(APPEND SOURCES
    ramdbRegistrationCreate.c
    ramdbRegistrationRetrieve.c
    ramdbRegistrationQuery.c
    ramdbRegistrationUpdate.c
    ramdbRegistrationDelete.c
  )
ENDIF()

ADD_LIBRARY(swRamDB SHARED ${SOURCES})

TARGET_COMPILE_DEFINITIONS(swRamDB PRIVATE PLUGIN_VERSION="0.2.0")
# Plugin .so resolves NGSI-LD/k-lib symbols from the (rdynamic) broker at dlopen.
TARGET_LINK_LIBRARIES(swRamDB geos_c m)

SET_TARGET_PROPERTIES(swRamDB PROPERTIES OUTPUT_NAME "swRamDB" PREFIX "")
