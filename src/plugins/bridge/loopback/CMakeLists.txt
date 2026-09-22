# SPDX-License-Identifier: Apache-2.0
ADD_LIBRARY(bridgeLoopback SHARED loopbackRegister.c)

TARGET_LINK_LIBRARIES(bridgeLoopback pthread)

SET_TARGET_PROPERTIES(bridgeLoopback PROPERTIES OUTPUT_NAME "loopback" PREFIX "")
