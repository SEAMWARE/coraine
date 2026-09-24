#ifndef TROE_TROEFROMMERGE_H_
#define TROE_TROEFROMMERGE_H_

//
// FILE            troeFromMerge.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Helper: walk an LdMergeReport's "changes" array and defer one
// TroeEvent per top-level attribute change. Maps the report's
// "attributeCreated"/"attributeModified"/"attributeDeleted" reasons
// to TroeOpAttrCreated/Modified/Deleted respectively.
//
// Allocates each TroeEvent from corRest.kalloc; lifetime is until the
// post-response dispatch.
//

#include <stdint.h>                                       // uint64_t

#include "kjson/KjNode.h"                                 // KjNode
#include "corNgsild/ldEntityMerge.h"                       // LdMergeReport
#include "db/Tenant.h"                                    // Tenant


extern void troeDeferAttrEventsFromMerge(Tenant*         tenantP,
                                         const char*     entityId,
                                         const char*     entityType,
                                         KjNode*         mergedEntity,
                                         LdMergeReport*  reportP,
                                         uint64_t        modifiedAtNs);




// -----------------------------------------------------------------------------
//
// troeDeferRemovedByReplace - a deletion event for every instance a Replace removed
//
// A Replace Entity writes the Attributes of its body - each has its row from
// the caller - and removes whatever else the entity had: a whole Attribute,
// or instances of one it kept. Those are deletions (§ 5.3.2.5: an instance
// with value urn:ngsi-ld:null and deletedAt), one per instance, each naming
// its datasetId. Both entities dataset-keyed (DB model).
//
extern void troeDeferRemovedByReplace(Tenant*      tenantP,
                                      const char*  entityId,
                                      const char*  entityType,
                                      KjNode*      oldEntity,
                                      KjNode*      newEntity,
                                      uint64_t     modifiedAtNs);

#endif  // TROE_TROEFROMMERGE_H_
