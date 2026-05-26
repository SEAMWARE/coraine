ADD_EXECUTABLE(swBroker swBroker.c ngsildServices.c)

SET_TARGET_PROPERTIES(swBroker PROPERTIES ENABLE_EXPORTS TRUE)

SET(SW_BROKER_LIBS serviceRoutines linkedEntities db troe plugin)
IF (SW_FEATURE_METRICS)
  LIST(APPEND SW_BROKER_LIBS metrics)
ENDIF()
IF (SW_FEATURE_REGISTRATIONS)
  LIST(APPEND SW_BROKER_LIBS forwarding)
ENDIF()

# The broker's own component libs are whole-archived: many of their symbols are
# referenced only via the dispatch table, constructor-registered hooks, or by
# dlopen'd plugins resolving against the (rdynamic) broker — a plain static link
# would strip those. The sw/k libs are whole-archived for the same reason.
TARGET_LINK_LIBRARIES(swBroker
  -Wl,--whole-archive ${SW_BROKER_LIBS} ${SW_STATIC_LIBS} -Wl,--no-whole-archive
  ${SW_DYNAMIC_LIBS}
)
