#
# ftClient — test notification receiver (for subscription tests)
#
ADD_EXECUTABLE(ftClient funcTests/ftClient/ftClient.c)
SET_TARGET_PROPERTIES(ftClient PROPERTIES
  ENABLE_EXPORTS TRUE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/test/funcTests/ftClient"
)
TARGET_LINK_LIBRARIES(ftClient
  -Wl,--whole-archive ${SW_STATIC_LIBS} -Wl,--no-whole-archive
  ${SW_DYNAMIC_LIBS}
)
