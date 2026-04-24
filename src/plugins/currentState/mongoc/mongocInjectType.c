//
// FILE            mongocInjectType.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                   // strcmp

#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjBuilder.h"                          // kjString, kjChildAdd
#include "kjson/kjNodeDecouple.h"                     // kjNodeDecouple

#include "swRest/SwRestState.h"                       // swRest

#include "currentState/mongoc/mongocInjectType.h"     // Own interface



// -----------------------------------------------------------------------------
//
// mongocStripTypeDecouple -
//
void mongocStripTypeDecouple(KjNode* treeP, KjNode** typePOut, KjNode** typePrevOut)
{
  *typePOut    = NULL;
  *typePrevOut = NULL;

  if (treeP == NULL || treeP->type != KjObject)
    return;

  KjNode* prev = NULL;
  for (KjNode* c = treeP->value.firstChildP; c != NULL; c = c->next)
  {
    if (c->name != NULL && strcmp(c->name, "type") == 0)
    {
      *typePOut    = c;
      *typePrevOut = prev;
      kjNodeDecouple(treeP, c, prev);
      return;
    }
    prev = c;
  }
}



// -----------------------------------------------------------------------------
//
// mongocStripTypeRestore -
//
void mongocStripTypeRestore(KjNode* treeP, KjNode* typeP, KjNode* typePrevP)
{
  if (typeP == NULL || treeP == NULL) return;

  if (typePrevP == NULL)
  {
    typeP->next = treeP->value.firstChildP;
    treeP->value.firstChildP = typeP;
    if (treeP->lastChild == NULL) treeP->lastChild = typeP;
  }
  else
  {
    typeP->next = typePrevP->next;
    typePrevP->next = typeP;
    if (typePrevP == treeP->lastChild) treeP->lastChild = typeP;
  }
}



// -----------------------------------------------------------------------------
//
// mongocInjectTypeAfterId -
//
void mongocInjectTypeAfterId(KjNode* objP, const char* typeValue)
{
  if (objP == NULL || objP->type != KjObject) return;
  if (kjLookup(objP, "type") != NULL)         return;

  KjNode* typeNode = kjString(swRest.kjsonP, "type", (char*) typeValue);
  KjNode* idP      = kjLookup(objP, "id");

  if (idP != NULL)
  {
    typeNode->next = idP->next;
    idP->next      = typeNode;
    if (objP->lastChild == idP) objP->lastChild = typeNode;
  }
  else
  {
    kjChildAdd(objP, typeNode);
  }
}
