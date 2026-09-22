# SPDX-License-Identifier: Apache-2.0
SET(SOURCES
  channelCache.c
  channelConfigLoad.c
  channelPrePopulate.c
)

ADD_LIBRARY(bridge STATIC ${SOURCES})
