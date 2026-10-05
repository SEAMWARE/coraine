# SPDX-License-Identifier: Apache-2.0
#
# ws.so - WebSocket as a transport of the broker's API (doc/websocket.md). SHA-1 and base64 from
# libcrypto, which every coraine with OpenSSL has loaded already.
#
ADD_LIBRARY(transportWs SHARED wsTransport.c)

TARGET_LINK_LIBRARIES(transportWs crypto pthread)

SET_TARGET_PROPERTIES(transportWs PROPERTIES OUTPUT_NAME "ws" PREFIX "")
