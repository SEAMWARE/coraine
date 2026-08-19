#ifndef GET_ENTITY_H
#define GET_ENTITY_H

//
// FILE            getEntity.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                              // bool

#include "kjson/KjNode.h"                          // KjNode
#include "db/Tenant.h"                             // Tenant



// -----------------------------------------------------------------------------
//
// getEntity -
//
extern bool getEntity(void);



// -----------------------------------------------------------------------------
//
// DistRetrieveErr - exclusive-source failure carried out of
// distributedRetrieveOne so the caller chooses how to react (502 for a
// top-level retrieve, "link unfollowed" for a join).
//
typedef struct DistRetrieveErr
{
  int          status;          // 0 = ok; 504 = single-source timeout, 502 = other single-source failure
  bool         noEndpoint;      // true: exclusive registration had no endpoint
  const char*  upstreamDetail;  // upstream error detail, or NULL
  const char*  regId;           // failing registration id
  int          upstreamCode;    // upstream status that triggered the 502, or 0
} DistRetrieveErr;



// -----------------------------------------------------------------------------
//
// distributedRetrieveOne - § 5.7.1.4 single-entity distributed assemble
// (local + type-matched registrations, merged per § 4.5.5.3). See the
// implementation in getEntity.c for the full contract.
//
extern KjNode* distributedRetrieveOne(const char* entityId, char** typeV, Tenant* tP,
                                      bool wholeForward, bool* matchedP, DistRetrieveErr* errP);

#endif  // GET_ENTITY_H
