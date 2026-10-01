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
)

ADD_LIBRARY(bridge STATIC ${SOURCES})
