# SPDX-License-Identifier: Apache-2.0
SET(SOURCES
  corNotInThisBuild.c
  getBuild.c
  getEntities.c
  getEntity.c
  postEntities.c
  deleteEntity.c
  patchEntity.c
  replaceEntity.c
  postEntityAttrs.c
  patchEntityAttrs.c
  purgeEntities.c
  postSnapshot.c
  getSnapshot.c
  deleteSnapshot.c
  ldSnapshotExec.c
  ldSnapshotExecTemporal.c
  ldSnapshotCaptureAsync.c
  ldSnapshotRead.c
  patchSnapshot.c
  cloneSnapshot.c
  purgeSnapshots.c
  getEntityAttr.c
  getEntityAttrValue.c
  patchEntityAttr.c
  putEntityAttr.c
  putEntityAttrValue.c
  deleteEntityAttr.c
  postEntityBatchCreate.c
  postEntityBatchUpdate.c
  postEntityBatchUpsert.c
  postEntityBatchMerge.c
  postEntityBatchDelete.c
  postEntityBatchQuery.c
  getTypes.c
  getType.c
  getAttributes.c
  getAttribute.c
  getSourceIdentity.c
  getVersion.c
  getEntityTemporal.c
  getEntitiesTemporal.c
  postTemporalEntityBatchQuery.c
  deleteEntityTemporal.c
  deleteEntityTemporalAttr.c
  postEntitiesTemporal.c
  postEntityTemporalAttrs.c
  patchEntityTemporalInstance.c
  deleteEntityTemporalInstance.c
)

IF (COR_FEATURE_CONTEXT_HOSTING)
  LIST(APPEND SOURCES
    getJsonldContexts.c
    getJsonldContext.c
    postJsonldContexts.c
    deleteJsonldContext.c
  )
ENDIF()

IF (COR_FEATURE_SUBSCRIPTIONS)
  LIST(APPEND SOURCES
    postSubscriptions.c
    getSubscription.c
    getSubscriptions.c
    patchSubscription.c
    deleteSubscription.c
    postExNotification.c
  )
ENDIF()

IF (COR_FEATURE_REGISTRATIONS)
  LIST(APPEND SOURCES
    regConflictCheck.c
    postCsourceRegistration.c
    getCsourceRegistration.c
    getCsourceRegistrations.c
    patchCsourceRegistration.c
    deleteCsourceRegistration.c
    getEntityMap.c
    deleteEntityMap.c
    patchEntityMap.c
    createEntityMap.c
    postEntityMap.c
  )
ENDIF()

IF (COR_FEATURE_SUBSCRIPTIONS AND COR_FEATURE_REGISTRATIONS)
  LIST(APPEND SOURCES
    postCsourceSubscriptions.c
    getCsourceSubscription.c
    getCsourceSubscriptions.c
    patchCsourceSubscription.c
    deleteCsourceSubscription.c
  )
ENDIF()

ADD_LIBRARY(serviceRoutines STATIC ${SOURCES})
