//
// FILE            troeInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                   // NULL

#include "ktrace/kTrace.h"                            // KT_E, KT_I

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
    KT_E("troe driver init failed (rc=%d)", r);
    return r;
  }

  if (troe.alias != NULL)
    KT_I("troe plugin online: %s", troe.alias);

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
