//
// FILE            timescaleRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Plugin entry point — fills in the TroeDriver function-pointer table.
//
#define PLUGIN_VERSION "0.1.0"

#include <stddef.h>                                       // NULL

#include "troe/TroeDriver.h"                              // TroeDriver

#include "temporal/timescale/timescaleGlobals.h"          // timescaleArgV
#include "temporal/timescale/timescaleInit.h"             // timescaleInit, timescaleClose
#include "temporal/timescale/timescaleEvent.h"            // timescaleEntityEvent, timescaleAttrEvent, timescaleEventList
#include "temporal/timescale/timescaleQuery.h"            // timescaleEntityTemporalRetrieve
#include "temporal/timescale/timescaleHistoryWrite.h"     // timescaleEntityTemporalDelete, etc.



// -----------------------------------------------------------------------------
//
// troeRegister -
//
void troeRegister(TroeDriver* driverP)
{
  driverP->alias        = "timescale";
  driverP->version      = PLUGIN_VERSION;
  driverP->args         = timescaleArgV;
  driverP->init         = timescaleInit;
  driverP->close        = timescaleClose;
  driverP->tenantSetup  = NULL;
  driverP->migrate      = NULL;  // run inline by init; no per-tenant work yet
  driverP->entityEvent  = timescaleEntityEvent;
  driverP->attrEvent    = timescaleAttrEvent;
  driverP->eventList    = timescaleEventList;
  driverP->entityTemporalQuery     = timescaleEntityTemporalQuery;
  driverP->entityTemporalRetrieve  = timescaleEntityTemporalRetrieve;
  driverP->entityTemporalDelete    = timescaleEntityTemporalDelete;
  driverP->entityTemporalAttrDelete = timescaleEntityTemporalAttrDelete;
  driverP->entityTemporalCreate    = timescaleEntityTemporalCreate;
  driverP->entityTemporalAttrsAdd  = timescaleEntityTemporalAttrsAdd;
  driverP->versionInfo  = NULL;
  driverP->dumpInfo     = NULL;
}
