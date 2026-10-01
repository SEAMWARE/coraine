# SPDX-License-Identifier: Apache-2.0
#
# ftClient — test notification receiver (for subscription tests)
#
ADD_EXECUTABLE(ftClient funcTests/ftClient/ftClient.c)
SET_TARGET_PROPERTIES(ftClient PROPERTIES
  ENABLE_EXPORTS TRUE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/test/funcTests/ftClient"
)
#
# mosquitto is ftClient's own: it subscribes to the MQTT broker the mqtt:// notification tests
# publish to. The broker itself no longer links it - MQTT is the mqtt.so bridge (corMqttBridge).
#
TARGET_LINK_LIBRARIES(ftClient
  -Wl,--whole-archive ${COR_STATIC_LIBS} -Wl,--no-whole-archive
  ${COR_DYNAMIC_LIBS}
  mosquitto
)

#
# corRequest - a cor:// client for tests and benchmarks: one request (like curl -i), or load (like wrk)
#
ADD_EXECUTABLE(corRequest funcTests/corRequest/corRequest.c)
SET_TARGET_PROPERTIES(corRequest PROPERTIES
  ENABLE_EXPORTS TRUE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/test/funcTests/corRequest"
)
TARGET_LINK_LIBRARIES(corRequest
  -Wl,--whole-archive ${COR_STATIC_LIBS} -Wl,--no-whole-archive
  ${COR_DYNAMIC_LIBS}
)
