#ifndef TIMESCALE_TIMESCALEHISTORYWRITE_H_
#define TIMESCALE_TIMESCALEHISTORYWRITE_H_

//
// FILE            timescaleHistoryWrite.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Direct write paths for the temporal-write endpoints (§ 5.6.12,
// § 5.6.13, § 5.6.16, § 5.6.11). Bypass the current-state DB and
// the event-deferral pipeline — the client is dictating history.
//

#include "kjson/KjNode.h"                                 // KjNode

#include "troe/TroeDriver.h"                              // TROE_*
#include "db/Tenant.h"                                    // Tenant


extern int timescaleEntityTemporalDelete(Tenant* tenantP, const char* entityId);

extern int timescaleEntityTemporalAttrDelete(Tenant* tenantP, const char* entityId,
                                             const char* attrName,
                                             const char* datasetId, bool deleteAll);

extern int timescaleEntityTemporalCreate(Tenant* tenantP, KjNode* rootP);

extern int timescaleEntityTemporalAttrsAdd(Tenant* tenantP, const char* entityId, KjNode* rootP);

extern int timescaleEntityTemporalInstanceModify(Tenant* tenantP,
                                                 const char* entityId,
                                                 const char* attrName,
                                                 const char* instanceId,
                                                 KjNode* rootP);

extern int timescaleEntityTemporalInstanceDelete(Tenant* tenantP,
                                                 const char* entityId,
                                                 const char* attrName,
                                                 const char* instanceId);


#endif  // TIMESCALE_TIMESCALEHISTORYWRITE_H_
