#ifndef TROE_TROEDRIVER_H_
#define TROE_TROEDRIVER_H_

//
// FILE            TroeDriver.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Plugin interface for Temporal Representation of Entities (NGSI-LD § 5.7.4 /
// § 5.6.16+). Mirrors the shape of db/DbDriver.h. The plugin owns hot/cold
// tier routing; the broker hands events one-at-a-time via the defer queue.
//

#include <stdint.h>                                       // uint64_t
#include <stdbool.h>                                      // bool

#include "kalloc/KAlloc.h"                                // KAlloc
#include "kargs/KArg.h"                                   // KArg
#include "kjson/KjNode.h"                                 // KjNode

#include "db/Tenant.h"                                    // Tenant



// -----------------------------------------------------------------------------
//
// Error codes — same convention as DbDriver.
//
#define TROE_OK         0
#define TROE_ERR        -1
#define TROE_NOT_FOUND  -2



// -----------------------------------------------------------------------------
//
// TroeOp - what kind of event is being recorded.
//
// Single timestamp per row; the op disambiguates "what does ts mean".
// Note: there is no 'observed' op — a value-confirmation with a fresh
// observedAt is still a 'modified' event with the new observedAt as a
// column. Distinct row-types only for state transitions.
//
typedef enum
{
  TroeOpEntityCreated  = 1,
  TroeOpEntityReplaced = 2,
  TroeOpEntityDeleted  = 3,
  TroeOpAttrCreated    = 11,
  TroeOpAttrModified   = 12,
  TroeOpAttrReplaced   = 13,
  TroeOpAttrDeleted    = 14
} TroeOp;



// -----------------------------------------------------------------------------
//
// TroeEvent - one queued event awaiting persistence.
//
// Filled at write time by service routines (via troeDeferEntityEvent /
// troeDeferAttrEvent) and consumed post-response by troeDispatchPending.
//
// Lifetime: the event and its referenced strings/trees must live until the
// post-response dispatch runs. Allocate from the per-request kalloc arena.
//
typedef struct TroeEvent
{
  TroeOp             op;
  Tenant*            tenantP;

  // Entity-level fields (always present)
  const char*        entityId;
  const char*        entityType;
  uint64_t           modifiedAtNs;     // broker-receipt time

  // Attribute-level fields (NULL on entity-level ops)
  const char*        attrName;          // expanded IRI
  const char*        datasetId;         // empty string for the default instance
  KjNode*            attrSnapshot;      // value + observedAt + sub-attrs (post-merge)

  // Optional entity-level snapshot (used for replaced / created when the
  // plugin wants the full body without re-fetching).
  KjNode*            entitySnapshot;

  struct TroeEvent*  next;
} TroeEvent;



// -----------------------------------------------------------------------------
//
// TroeQueryFilter - compiled form of all temporal-query URL params.
//
// Filled in by the temporal-query service routine, consumed by the plugin's
// entityTemporalQuery / entityTemporalRetrieve. Skeleton only — fields
// arrive as the read path lands.
//
typedef struct TroeQueryFilter
{
  // Time window (§ 5.7.4.4 — timerel + timeAt + endTimeAt)
  const char*  timerel;       // "before" | "after" | "between" — NULL = no time filter
  const char*  timeAtIso;     // raw ISO8601 (parameterised into SQL as ::timestamptz)
  const char*  endTimeAtIso;  // raw ISO8601 (only when timerel=between)

  // Which timestamp axis: "observedAt" (default) | "modifiedAt" | "createdAt"
  const char*  timeproperty;

  // ?attrs= — NULL-terminated array of expanded IRIs, NULL when no filter
  char**       attrV;

  // ?lastN= per-attribute instance cap (0 = no cap)
  int          lastN;

  // q-tree, geo, ids, types — grow as the read path lands
  void*        opaque;
} TroeQueryFilter;



// -----------------------------------------------------------------------------
//
// Function pointer types
//
typedef int  (*TroeInitFunc)(void);
typedef void (*TroeCloseFunc)(void);
typedef int  (*TroeTenantSetupFunc)(Tenant* tenantP);
typedef int  (*TroeMigrateFunc)(Tenant* tenantP);

//
// One-event write entry-points. The plugin chooses whether to batch
// internally (e.g. a per-thread bulk that flushes at end-of-dispatch).
//
typedef int  (*TroeAttrEventFunc)(const TroeEvent* evP);
typedef int  (*TroeEntityEventFunc)(const TroeEvent* evP);

//
// Bulk variant — drained as a single linked list per request. Drivers
// that benefit from one-shot writes (timescale COPY, parquet append) use
// this; simple drivers can ignore it and rely on the per-event path.
//
typedef int  (*TroeEventListFunc)(const TroeEvent* listHead, int count);

//
// Read entry-points.
//
typedef int  (*TroeEntityTemporalQueryFunc)(Tenant* tenantP, TroeQueryFilter* fP, KjNode** resultPP);
typedef int  (*TroeEntityTemporalRetrieveFunc)(Tenant* tenantP, const char* entityId,
                                               TroeQueryFilter* fP, KjNode** resultPP);

typedef void (*TroeVersionInfoFunc)(KAlloc* allocP, KjNode* root);

//
// Dev/test helper: dump recent events captured by the plugin (e.g. ramdb).
// Production plugins (timescale, parquet) leave it NULL — admin route
// returns 501. Allocates onto allocP, appends children to root.
//
typedef void (*TroeDumpInfoFunc)(KAlloc* allocP, KjNode* root);



// -----------------------------------------------------------------------------
//
// TroeDriver -
//
typedef struct TroeDriver
{
  const char*                       alias;        // short plugin name (e.g. "timescale")
  const char*                       version;
  KArg*                             args;         // plugin-contributed CLI args (NULL if none)

  TroeInitFunc                      init;
  TroeCloseFunc                     close;
  TroeTenantSetupFunc               tenantSetup;
  TroeMigrateFunc                   migrate;      // applies pending schema migrations

  TroeEntityEventFunc               entityEvent;
  TroeAttrEventFunc                 attrEvent;
  TroeEventListFunc                 eventList;    // optional bulk drain; NULL → per-event path used

  TroeEntityTemporalQueryFunc       entityTemporalQuery;
  TroeEntityTemporalRetrieveFunc    entityTemporalRetrieve;

  TroeVersionInfoFunc               versionInfo;
  TroeDumpInfoFunc                  dumpInfo;       // dev/test only — NULL for prod plugins
} TroeDriver;



// -----------------------------------------------------------------------------
//
// TroeRegisterFunc -
//
typedef void (*TroeRegisterFunc)(TroeDriver* driverP);



// -----------------------------------------------------------------------------
//
// troe - global driver instance (filled by pluginLoadTroe via troeRegister)
//
extern TroeDriver troe;

#endif  // TROE_TROEDRIVER_H_
