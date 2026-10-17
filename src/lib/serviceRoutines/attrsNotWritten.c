//
// FILE            attrsNotWritten.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool

#include "corAlloc/corAllocStrdup.h"                  // corAllocStrdup
#include "corRest/CorRestState.h"                     // corRest

#include "serviceRoutines/AttrsNotWritten.h"          // Own interface



// -----------------------------------------------------------------------------
//
// attrsNotWrittenAdd -
//
bool attrsNotWrittenAdd(AttrsNotWritten* listP, const char* attr, int status, const char* type, const char* title, const char* reason)
{
  if (listP->n >= (int) (sizeof(listP->attrV) / sizeof(listP->attrV[0])))
    return false;

  listP->attrV[listP->n]   = attr;
  listP->statusV[listP->n] = status;
  listP->reasonV[listP->n] = corAllocStrdup(&corRest.kalloc, reason);
  listP->titleV[listP->n]  = title;
  listP->typeV[listP->n]   = type;
  listP->n++;

  return true;
}
