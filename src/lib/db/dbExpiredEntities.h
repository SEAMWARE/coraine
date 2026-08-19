#ifndef CORAINE_DBEXPIREDENTITIES_H_
#define CORAINE_DBEXPIREDENTITIES_H_

//
// FILE            dbExpiredEntities.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdbool.h>                                 // bool

#include "kjson/KjNode.h"                            // KjNode

#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// dbExpiredEntityDefer - remember an expired Entity to delete after responding
//
// § 5.2.4: at expiresAt an Entity "shall become invalid and may be
// automatically removed". There is no reaper thread — an Entity is noticed
// when a read walks over it, dropped from that response, and deleted once the
// response is on its way. The read paths deliberately do NOT filter on
// expiresAt in the database: the rows have to reach RAM for the broker to see
// which ones to remove.
//
// entityId is copied into the request arena, so the caller's node may go away.
//
extern void dbExpiredEntityDefer(Tenant* tenantP, const char* entityId);



// -----------------------------------------------------------------------------
//
// dbExpiredEntityDispatchPending - delete what the request found expired
//
// Called from the post-response hook, next to the notification and TRoE
// drains. Deleting after the response keeps the read fast and means a failed
// delete never affects what the client was told.
//
extern void dbExpiredEntityDispatchPending(void);



// -----------------------------------------------------------------------------
//
// dbExpiredEntityIs - has this Entity's entity-level expiresAt passed?
//
// Defers the Entity for deletion as a side effect when it has, so a caller can
// simply skip it. entityP is in storage format; expiresAt sits at top level in
// either the nanosecond-integer or the ISO form.
//
extern bool dbExpiredEntityIs(Tenant* tenantP, KjNode* entityP);



// -----------------------------------------------------------------------------
//
// dbExpiredEntityFilter - drop expired Entities from a query result array
//
// Each one removed is deferred for deletion. Runs before pagination, counting
// and any distributed merge, so an expired Entity is invisible to all of them.
//
extern void dbExpiredEntityFilter(Tenant* tenantP, KjNode* arrayP);

#endif  // CORAINE_DBEXPIREDENTITIES_H_
