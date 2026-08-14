ADD_EXECUTABLE(swBroker swBroker.c ngsildServices.c)

SET_TARGET_PROPERTIES(swBroker PROPERTIES ENABLE_EXPORTS TRUE)

SET(SW_BROKER_LIBS serviceRoutines linkedEntities db ha troe plugin)
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

# The sibling-repo static libs (${SW_STATIC_LIBS}) are passed inside a
# -Wl,--whole-archive ... linker-flag string, so CMake sees them as opaque flags
# rather than file inputs and won't relink when one changes (editing only a lib's
# source then rebuilding the broker would silently keep the stale archive linked).
# LINK_DEPENDS makes the link rule depend on the .a files explicitly, so a changed
# sibling lib forces a relink. (The in-tree component libs in ${SW_BROKER_LIBS}
# are real CMake targets and are already tracked.)
SET_TARGET_PROPERTIES(swBroker PROPERTIES LINK_DEPENDS "${SW_STATIC_LIBS}")
