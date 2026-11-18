#ifndef SRC_LIB_SERVICEROUTINES_SERVICEEXECUTOR_H_
#define SRC_LIB_SERVICEROUTINES_SERVICEEXECUTOR_H_

//
// FILE            serviceExecutor.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool



// -----------------------------------------------------------------------------
//
// postServiceExecutor - POST /ngsi-ld/ex/v1/executor/{serviceName} - this broker as another's executor
//
extern bool postServiceExecutor(void);



// -----------------------------------------------------------------------------
//
// deleteServiceExecutor - DELETE /ngsi-ld/ex/v1/executor/{serviceName}/{executionId} - its cancel
//
extern bool deleteServiceExecutor(void);

#endif  // SRC_LIB_SERVICEROUTINES_SERVICEEXECUTOR_H_
