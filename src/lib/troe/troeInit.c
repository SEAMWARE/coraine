//
// FILE            troeInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                   // NULL

#include "corLog/corLog.h"                            // COR_E, COR_I

#include "troe/TroeDriver.h"                          // TroeDriver
#include "troe/troeInit.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// troe - global driver instance (filled by pluginLoadTroe via troeRegister)
//
TroeDriver troe;



// -----------------------------------------------------------------------------
//
// troeStart - call troe.init() to bring the plugin online
//
// The "none" plugin's init is a no-op returning TROE_OK; real backends
// (timescale, parquet) connect / open files / run pending migrations here.
//
int troeStart(void)
{
  if (troe.init == NULL)
  {
    // No troe plugin loaded — silent. Caller already knows.
    return TROE_OK;
  }

  int r = troe.init();
  if (r != TROE_OK)
  {
    COR_E("troe driver init failed (rc=%d)", r);
    return r;
  }

  if (troe.alias != NULL)
    COR_I("troe plugin online: %s", troe.alias);

  return TROE_OK;
}



// -----------------------------------------------------------------------------
//
// troeStop - call troe.close() at shutdown
//
void troeStop(void)
{
  if (troe.close != NULL)
    troe.close();
}
