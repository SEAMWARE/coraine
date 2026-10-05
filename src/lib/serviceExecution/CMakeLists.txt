# SPDX-License-Identifier: Apache-2.0
#
# Service Execution (doc/service-execution.md) - built only with COR_FEATURE_SERVICE_EXECUTION
#
SET(SOURCES
  seCoreTerms.c
  seRegistrationCheck.c
  seRegistrationMatch.c
  seRegistrationRender.c
)

ADD_LIBRARY(serviceExecution STATIC ${SOURCES})
