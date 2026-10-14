# SPDX-License-Identifier: Apache-2.0
SET(SOURCES
  channelCache.c
  channelConfigLoad.c
  channelPrePopulate.c
  bridgeSampleIn.c
  bridgeServiceSync.c
  bridgeGoal.c
  bridgeDefaultEntity.c
  bridgeCoreTerms.c
  bridgeRender.c
  bridgeNotify.c
  bridgeService.c
)

IF (COR_FEATURE_DATA_CHANNELS)
  LIST(APPEND SOURCES recordMap.c dataChannelCache.c dataChannelRecordIn.c dataChannelRender.c)
ENDIF()

ADD_LIBRARY(bridge STATIC ${SOURCES})
