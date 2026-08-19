# SPDX-License-Identifier: Apache-2.0
FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(LIBPQ REQUIRED libpq)

SET(TIMESCALE_SOURCES
  timescaleGlobals.c
  timescalePool.c
  timescaleInit.c
  timescaleMigrate.c
  timescaleEvent.c
  timescaleQuery.c
  timescaleHistoryWrite.c
  timescaleRegister.c
)

ADD_LIBRARY(troeTimescale SHARED ${TIMESCALE_SOURCES})
TARGET_INCLUDE_DIRECTORIES(troeTimescale PRIVATE ${LIBPQ_INCLUDE_DIRS})
# Plugin .so resolves NGSI-LD/k-lib symbols from the (rdynamic) broker at dlopen,
# so only its own external deps are linked here.
TARGET_LINK_LIBRARIES(troeTimescale ${LIBPQ_LIBRARIES} pthread)
SET_TARGET_PROPERTIES(troeTimescale PROPERTIES OUTPUT_NAME "timescale" PREFIX "")
