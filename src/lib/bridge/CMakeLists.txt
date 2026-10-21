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

IF (COR_FEATURE_CHANNEL_CHECKS)
  LIST(APPEND SOURCES channelCheck.c)
ENDIF()

IF (COR_FEATURE_BRIDGE_HTTP_LISTEN)
  LIST(APPEND SOURCES bridgeHttpListen.c)
ENDIF()

IF (COR_FEATURE_DATA_CHANNELS)
  LIST(APPEND SOURCES recordMap.c dataChannelCache.c dataChannelRecordIn.c dataChannelRender.c)
ENDIF()

IF (COR_FEATURE_CHANNEL_WRITE_BACK)
  LIST(APPEND SOURCES dataChannelWriteBack.c)
ENDIF()

IF (COR_FEATURE_ASSET_CHANNELS)
  LIST(APPEND SOURCES assetMap.c assetChannelCache.c assetChannelSampleIn.c assetChannelRender.c)
ENDIF()

IF (COR_FEATURE_DATA_CHANNELS OR COR_FEATURE_ASSET_CHANNELS)
  LIST(APPEND SOURCES bridgeEntitiesUpsert.c)
ENDIF()

ADD_LIBRARY(bridge STATIC ${SOURCES})
