//
// FILE            noneRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// "none" troe plugin — TRoE disabled. All entry-points are no-ops.
// Used by --troe=none (the default) so the broker can run without any
// temporal storage configured.
//
#define PLUGIN_VERSION "0.1.0"

#include <stddef.h>                                       // NULL

#include "troe/TroeDriver.h"                              // TroeDriver



// -----------------------------------------------------------------------------
//
// noneInit / noneClose -
//
static int  noneInit(void)   { return TROE_OK; }
static void noneClose(void)  { }



// -----------------------------------------------------------------------------
//
// troeRegister -
//
void troeRegister(TroeDriver* driverP)
{
  driverP->alias        = "none";
  driverP->version      = PLUGIN_VERSION;
  driverP->args         = NULL;
  driverP->init         = noneInit;
  driverP->close        = noneClose;
  driverP->tenantSetup  = NULL;
  driverP->migrate      = NULL;
  driverP->entityEvent  = NULL;
  driverP->attrEvent    = NULL;
  driverP->eventList    = NULL;
  driverP->entityTemporalQuery    = NULL;
  driverP->entityTemporalRetrieve = NULL;
  driverP->versionInfo  = NULL;
}
