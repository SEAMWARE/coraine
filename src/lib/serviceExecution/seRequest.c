//
// FILE            seRequest.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <string.h>                                   // strcmp
#include <strings.h>                                  // strcasecmp

#include "corRest/CorRestState.h"                     // corRest

#include "serviceExecution/seRequest.h"               // Own interface



// -----------------------------------------------------------------------------
//
// seRequestHeader -
//
const char* seRequestHeader(const char* key)
{
  for (int ix = 0; ix < corRest.in.httpHeaderCount; ix++)
  {
    if (strcasecmp(corRest.in.httpHeaderV[ix].key, key) == 0)
      return corRest.in.httpHeaderV[ix].value;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// seRequestParam -
//
const char* seRequestParam(const char* key)
{
  for (int ix = 0; ix < corRest.in.uriParamCount; ix++)
  {
    if (strcmp(corRest.in.uriParamV[ix].key, key) == 0)
      return corRest.in.uriParamV[ix].value;
  }

  return NULL;
}
