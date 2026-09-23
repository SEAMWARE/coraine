# SPDX-License-Identifier: Apache-2.0
SET(SOURCES
  channelCache.c
  channelConfigLoad.c
  channelPrePopulate.c
  bridgeSampleIn.c
  bridgeAttrOut.c
  bridgeAttrsOut.c
  bridgeServiceSync.c
  bridgeDefaultEntity.c
)

ADD_LIBRARY(bridge STATIC ${SOURCES})
