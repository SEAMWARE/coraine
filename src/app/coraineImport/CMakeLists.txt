# SPDX-License-Identifier: Apache-2.0
#
# coraine-import - the writer half of a migration (doc/migration.md). A program of its own: linked
# against the broker's libraries, as the broker is (whole-archive, exported - the store plugins it
# loads resolve their symbols against it), but not the broker's main - the broker carries no import code.
#
ADD_EXECUTABLE(coraine-import coraineImport.c ../coraine/coraineFeatures.c)

SET_TARGET_PROPERTIES(coraine-import PROPERTIES ENABLE_EXPORTS TRUE)

SET(COR_IMPORT_LIBS migrate serviceRoutines linkedEntities db ha troe plugin startup bridge)
IF (COR_FEATURE_METRICS)
  LIST(APPEND COR_IMPORT_LIBS metrics)
ENDIF()
IF (COR_FEATURE_TRANSPORTS)
  LIST(APPEND COR_IMPORT_LIBS transport)
ENDIF()
IF (COR_FEATURE_REGISTRATIONS)
  LIST(APPEND COR_IMPORT_LIBS forwarding)
ENDIF()
IF (COR_FEATURE_SERVICE_EXECUTION)
  LIST(APPEND COR_IMPORT_LIBS serviceExecution)
ENDIF()

TARGET_LINK_LIBRARIES(coraine-import
  -Wl,--whole-archive ${COR_IMPORT_LIBS} ${COR_STATIC_LIBS} -Wl,--no-whole-archive
  ${COR_DYNAMIC_LIBS}
)

SET_TARGET_PROPERTIES(coraine-import PROPERTIES LINK_DEPENDS "${COR_STATIC_LIBS}")
