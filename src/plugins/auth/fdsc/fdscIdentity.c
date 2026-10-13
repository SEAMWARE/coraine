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
  idP->exp         = 0;
  idP->nbf         = 0;
  idP->refs        = 1;
  return idP;
}



// -----------------------------------------------------------------------------
//
// fdscIdentityFree -
//
void fdscIdentityFree(FdscIdentity* idP)
{
  fdscIdentityUnref(idP);
}



// -----------------------------------------------------------------------------
//
// fdscIdentityRef / fdscIdentityUnref -
//
void fdscIdentityRef(FdscIdentity* idP)
{
  if (idP != NULL)
    __atomic_add_fetch(&idP->refs, 1, __ATOMIC_RELAXED);
}

void fdscIdentityUnref(FdscIdentity* idP)
{
  if ((idP == NULL) || (__atomic_sub_fetch(&idP->refs, 1, __ATOMIC_ACQ_REL) > 0))
    return;

  corAllocBufferReset(&idP->ka, false);
  free(idP);
}



// -----------------------------------------------------------------------------
//
// fdscHandleNew / fdscHandleFree -
//
FdscHandle* fdscHandleNew(FdscIdentity* idP)
{
  FdscHandle* hP = (FdscHandle*) malloc(sizeof(FdscHandle));

  if (hP == NULL)
  {
    fdscIdentityUnref(idP);
    return NULL;
  }

  hP->idP         = idP;
  hP->entityFree  = false;
  hP->falseSet    = 0;
  hP->falseMethod = 0;
  hP->falseMask   = 0;
  return hP;
}

void fdscHandleFree(FdscHandle* hP)
{
  if (hP == NULL)
    return;

  fdscIdentityUnref(hP->idP);
  free(hP);
}



// -----------------------------------------------------------------------------
//
// fdscIdentityFill -
//
void fdscIdentityFill(FdscIdentity* idP, CorNode* claimsP)
{
  idP->claimsP     = claimsP;
  idP->credentials = 0;

  CorNode* expP = corTreeLookup(claimsP, "exp");
  CorNode* nbfP = corTreeLookup(claimsP, "nbf");

  idP->exp = (expP == NULL) ? 0 : (expP->type == CorInt) ? expP->value.i : (expP->type == CorFloat) ? (long long) expP->value.f : 0;
  idP->nbf = (nbfP == NULL) ? 0 : (nbfP->type == CorInt) ? nbfP->value.i : (nbfP->type == CorFloat) ? (long long) nbfP->value.f : 0;

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
