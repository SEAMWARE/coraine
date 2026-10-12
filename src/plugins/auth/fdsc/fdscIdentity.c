//
// FILE            fdscIdentity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                                  // malloc, free

#include "corAlloc/corAllocBufferInit.h"             // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"            // corAllocBufferReset
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "fdscIdentity.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// fdscIdentityNew -
//
FdscIdentity* fdscIdentityNew(void)
{
  FdscIdentity* idP = (FdscIdentity*) malloc(sizeof(FdscIdentity));

  if (idP == NULL)
    return NULL;

  corAllocBufferInit(&idP->ka, idP->buf, sizeof(idP->buf), 8192, NULL, "fdsc identity");
  idP->claimsP     = NULL;
  idP->credentials = 0;
  idP->entityFree  = false;
  return idP;
}



// -----------------------------------------------------------------------------
//
// fdscIdentityFree -
//
void fdscIdentityFree(FdscIdentity* idP)
{
  if (idP == NULL)
    return;

  corAllocBufferReset(&idP->ka, false);
  free(idP);
}



// -----------------------------------------------------------------------------
//
// fdscIdentityFill -
//
void fdscIdentityFill(FdscIdentity* idP, CorNode* claimsP)
{
  idP->claimsP     = claimsP;
  idP->credentials = 0;

  CorNode* vcP = corTreeLookup(claimsP, "verifiableCredential");
  CorNode* vpP = corTreeLookup(claimsP, "verifiablePresentation");

  if ((vcP != NULL) && (vcP->type == CorObject))
    idP->credentialV[idP->credentials++] = vcP;

  if ((vpP != NULL) && (vpP->type == CorArray))
  {
    for (CorNode* cP = vpP->value.head; (cP != NULL) && (idP->credentials < 8); cP = cP->next)
    {
      if (cP->type == CorObject)
        idP->credentialV[idP->credentials++] = cP;
    }
  }
}
