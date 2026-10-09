#ifndef SRC_LIB_SERVICEEXECUTION_SEREQUEST_H_
#define SRC_LIB_SERVICEEXECUTION_SEREQUEST_H_

//
// FILE            seRequest.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The request's own parts that Service Execution reads - a header, a URL parameter.
//
// SE_PARAM_* - Service Execution's URL parameters, registered by the broker (corRestParamAdd).
// LD_PARAM_* grow from bit 0 (55 now), the broker's own from the top: ddsSync 63 (bridgeServiceSync.h),
// the admin plugin 62 (adminRegister.c), GET /entities' page position 59 (getEntities.h). Three are 56-58: the three filters of GET /services
// share one - they are allowed on that route only, and together.
//
#define SE_PARAM_INCLUDE_SERVICES  (1ULL << 56)   // GET /entities, /entities/{id}
#define SE_PARAM_SERVICE_DETAILS   (1ULL << 57)   // GET /entities, /entities/{id}
#define SE_PARAM_SERVICES_QUERY    (1ULL << 58)   // GET /services: entityId, serviceName, executionStatus



// -----------------------------------------------------------------------------
//
// seRequestHeader - a request header's value, NULL if absent (case-insensitive)
//
extern const char* seRequestHeader(const char* key);



// -----------------------------------------------------------------------------
//
// seRequestParam - a URL parameter's value, NULL if absent
//
extern const char* seRequestParam(const char* key);

#endif  // SRC_LIB_SERVICEEXECUTION_SEREQUEST_H_
