//
// FILE            mongocInjectType.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <string.h>                                   // strcmp

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBuilder.h"                   // corTreeString, corTreeChildAdd
#include "corTree/corTreeNodeDecouple.h"              // corTreeNodeDecouple

#include "corRest/CorRestState.h"                       // corRest

#include "currentState/mongoc/mongocInjectType.h"     // Own interface



// -----------------------------------------------------------------------------
//
// mongocStripTypeDecouple -
//
void mongocStripTypeDecouple(CorNode* treeP, CorNode** typePOut, CorNode** typePrevOut)
{
  *typePOut    = NULL;
  *typePrevOut = NULL;

  if (treeP == NULL || treeP->type != CorObject)
    return;

  CorNode* prev = NULL;
  for (CorNode* c = treeP->value.head; c != NULL; c = c->next)
  {
    if (c->name != NULL && strcmp(c->name, "type") == 0)
    {
      *typePOut    = c;
      *typePrevOut = prev;
      corTreeNodeDecouple(treeP, c, prev);
      return;
    }
    prev = c;
  }
}



// -----------------------------------------------------------------------------
//
// mongocStripTypeRestore -
//
void mongocStripTypeRestore(CorNode* treeP, CorNode* typeP, CorNode* typePrevP)
{
  if (typeP == NULL || treeP == NULL) return;

  if (typePrevP == NULL)
  {
    typeP->next = treeP->value.head;
    treeP->value.head = typeP;
    if (treeP->value.tail == NULL) treeP->value.tail = typeP;
  }
  else
  {
    typeP->next = typePrevP->next;
    typePrevP->next = typeP;
    if (typePrevP == treeP->value.tail) treeP->value.tail = typeP;
  }
}



// -----------------------------------------------------------------------------
//
// mongocInjectTypeAfterId -
//
void mongocInjectTypeAfterId(CorNode* objP, const char* typeValue)
{
  if (objP == NULL || objP->type != CorObject) return;
  if (corTreeLookup(objP, "type") != NULL)    return;

  CorNode* typeNode = corTreeString(corRest.kallocP, "type", (char*) typeValue);
  CorNode* idP     = corTreeLookup(objP, "id");

  if (idP != NULL)
  {
    typeNode->next = idP->next;
    idP->next      = typeNode;
    if (objP->value.tail == idP) objP->value.tail = typeNode;
  }
  else
  {
    corTreeChildAdd(objP, typeNode);
  }
}
