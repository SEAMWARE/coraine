#ifndef TROE_TROEFROMMERGE_H_
#define TROE_TROEFROMMERGE_H_

//
// FILE            troeFromMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Helper: walk an LdMergeReport's "changes" array and defer one
// TroeEvent per top-level attribute change. Maps the report's
// "attributeCreated"/"attributeModified"/"attributeDeleted" reasons
// to TroeOpAttrCreated/Modified/Deleted respectively.
//
// Allocates each TroeEvent from swRest.kalloc; lifetime is until the
// post-response dispatch.
//

#include <stdint.h>                                       // uint64_t

#include "kjson/KjNode.h"                                 // KjNode
#include "swNgsild/ldEntityMerge.h"                       // LdMergeReport
#include "db/Tenant.h"                                    // Tenant


extern void troeDeferAttrEventsFromMerge(Tenant*         tenantP,
                                         const char*     entityId,
                                         const char*     entityType,
                                         KjNode*         mergedEntity,
                                         LdMergeReport*  reportP,
                                         uint64_t        modifiedAtNs);

#endif  // TROE_TROEFROMMERGE_H_
