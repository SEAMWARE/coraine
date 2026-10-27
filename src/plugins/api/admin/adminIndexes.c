//
// FILE            adminIndexes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// /admin/indexes - the declared indexes of the store (doc/installation.md, "Declared indexes"): the
// attributes whose q value terms an index serves. The store does the work (DbDriver index*); a store
// without declared indexes answers 501.
//
//   GET    /admin/indexes        every declaration, with each tenant's index: built, bytes, accesses
//   POST   /admin/indexes        { "attribute": , "type"?: , "tenant"?: } - names expanded with the
//                                request's @context (Link header), the core one without
//   DELETE /admin/indexes/{id}   a declaration of the admin API taken away
//
// /admin/troeIndexes: the same for the history - the TRoE store's (troe.index*), timescale's
// --troeValueIndexes; its indexes made in the database too (declaredBy "database")
//
#include <stddef.h>                               // NULL
#include <stdio.h>                                // snprintf

#include "corTree/CorNode.h"                      // CorNode
#include "corTree/corTreeLookup.h"                // corTreeLookup
#include "corAlloc/corAllocStrdup.h"              // corAllocStrdup
#include "corJsonld/corLdCompact.h"               // corLdCompact
#include "corJsonld/corLdExpand.h"                // corLdExpand
#include "corJsonld/corLdInit.h"                  // corLdCoreContext
#include "corRest/CorRestState.h"                 // corRest
#include "corRest/corRestOutHeader.h"             // corRestOutHeaderAdd
#include "corNgsild/corNgsild.h"                  // corNgsild, ldError, ldContextResolve, LD_ERROR_*

#include "db/DbDriver.h"                          // db, DB_*
#include "troe/TroeDriver.h"                      // troe

#include "api/admin/adminIndexes.h"               // Own interface



// -----------------------------------------------------------------------------
//
// IndexStore - the store of a resource: the current state's (db) or the history's (troe)
//
typedef struct IndexStore
{
  const char*        alias;
  DbIndexListFunc    list;
  DbIndexAddFunc     add;
  DbIndexDeleteFunc  del;
  const char*        path;               // for Location
} IndexStore;

static IndexStore currentState(void) { IndexStore s = { db.alias,   db.indexList,   db.indexAdd,   db.indexDelete,   "/admin/indexes"     }; return s; }
static IndexStore history(void)      { IndexStore s = { troe.alias, troe.indexList, troe.indexAdd, troe.indexDelete, "/admin/troeIndexes" }; return s; }



// -----------------------------------------------------------------------------
//
// storeHasIndexes - false: 501 said
//
static bool storeHasIndexes(IndexStore* sP)
{
  if ((sP->list != NULL) && (sP->add != NULL) && (sP->del != NULL))
    return true;

  ldError(501, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented", "the store '%s' has no declared indexes", (sP->alias != NULL)? sP->alias : "?");
  return false;
}



// -----------------------------------------------------------------------------
//
// errorFromDb - a store's DB_* and its text as the response
//
static void errorFromDb(int rc, const char* errorText)
{
  switch (rc)
  {
  case DB_NOT_FOUND:       ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",   "%s", errorText); break;
  case DB_ALREADY_EXISTS:  ldError(409, LD_ERROR_ALREADY_EXISTS,     "Already Exists", "%s", errorText); break;
  case DB_BAD_INPUT:       ldError(409, LD_ERROR_CONFLICT,           "Conflict",    "%s", errorText); break;
  default:                 ldError(500, LD_ERROR_INTERNAL_ERROR,     "Internal Error", "%s", errorText); break;
  }
}



// -----------------------------------------------------------------------------
//
// declCompact - a declaration's attribute as the request's @context names it (its type is, by the
// rendering of every "type")
//
static void declCompact(CorNode* declP)
{
  CorNode* attrP = corTreeLookup(declP, "attribute");

  if ((attrP != NULL) && (attrP->type == CorString))
    attrP->value.s = (char*) corLdCompact((corNgsild.contextP != NULL)? corNgsild.contextP : corLdCoreContext(), attrP->value.s, &corRest.kalloc);
}



// -----------------------------------------------------------------------------
//
// indexesGet -
//
static bool indexesGet(IndexStore* sP)
{
  CorNode* arrayP = NULL;

  if (storeHasIndexes(sP) == false)
    return true;

  sP->list(&arrayP);

  ldContextResolve();
  for (CorNode* declP = arrayP->value.head; declP != NULL; declP = declP->next)
    declCompact(declP);

  corRest.out.responseTree = arrayP;
  corNgsild.rawResponse    = true;                    // not an Entity
  return true;
}



// -----------------------------------------------------------------------------
//
// indexesPost -
//
static bool indexesPost(IndexStore* sP)
{
  CorNode* bodyP = corRest.in.requestTree;

  if (storeHasIndexes(sP) == false)
    return true;

  CorNode* attrP   = (bodyP != NULL)? corTreeLookup(bodyP, "attribute") : NULL;
  CorNode* typeP   = (bodyP != NULL)? corTreeLookup(bodyP, "type")      : NULL;
  CorNode* tenantP = (bodyP != NULL)? corTreeLookup(bodyP, "tenant")    : NULL;

  if ((bodyP == NULL) || (bodyP->type != CorObject) || (attrP == NULL) || (attrP->type != CorString) || (attrP->value.s[0] == 0) ||
      ((typeP != NULL) && ((typeP->type != CorString) || (typeP->value.s[0] == 0))) ||
      ((tenantP != NULL) && ((tenantP->type != CorString) || (tenantP->value.s[0] == 0))))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "the body is { \"attribute\": <name>, \"type\"?: <entity type>, \"tenant\"?: <tenant> }");
    return true;
  }

  for (CorNode* nP = bodyP->value.head; nP != NULL; nP = nP->next)
  {
    if ((nP != attrP) && (nP != typeP) && (nP != tenantP))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Bad Request", "'%s': not a member of a declared index (attribute, type, tenant)", nP->name);
      return true;
    }
  }

  ldContextResolve();

  CorLdContext* ctxP    = (corNgsild.contextP != NULL)? corNgsild.contextP : corLdCoreContext();
  const char*   attrIri = corLdExpand(ctxP, attrP->value.s, &corRest.kalloc, NULL, NULL);
  const char*   typeIri = (typeP != NULL)? corLdExpand(ctxP, typeP->value.s, &corRest.kalloc, NULL, NULL) : NULL;

  if (attrIri == NULL) attrIri = attrP->value.s;
  if ((typeP != NULL) && (typeIri == NULL)) typeIri = typeP->value.s;

  char      errorText[512];
  CorNode*  declP = NULL;
  int       rc    = sP->add(attrIri, typeIri, (tenantP != NULL)? tenantP->value.s : NULL, &declP, errorText, sizeof(errorText));

  if (rc != DB_OK)
  {
    errorFromDb(rc, errorText);
    return true;
  }

  CorNode* idP = corTreeLookup(declP, "id");
  char     location[64];

  snprintf(location, sizeof(location), "%s/%s", sP->path, ((idP != NULL) && (idP->type == CorString))? idP->value.s : "");
  corRestOutHeaderAdd("Location", corAllocStrdup(&corRest.kalloc, location));   // the header keeps the pointer

  declCompact(declP);

  corRest.out.httpStatusCode = 201;
  corRest.out.responseTree   = declP;
  corNgsild.rawResponse      = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// indexDelete -
//
static bool indexDelete(IndexStore* sP)
{
  if (storeHasIndexes(sP) == false)
    return true;

  char errorText[512];
  int  rc = sP->del(corRest.in.wildcard[0], errorText, sizeof(errorText));

  if (rc != DB_OK)
    errorFromDb(rc, errorText);
  else
    corRest.out.httpStatusCode = 204;

  return true;
}



// -----------------------------------------------------------------------------
//
// The service routines: /admin/indexes - the current state's; /admin/troeIndexes - the history's
//
bool adminGetIndexes(void)      { IndexStore s = currentState(); return indexesGet(&s);  }
bool adminPostIndexes(void)     { IndexStore s = currentState(); return indexesPost(&s); }
bool adminDeleteIndex(void)     { IndexStore s = currentState(); return indexDelete(&s); }
bool adminGetTroeIndexes(void)  { IndexStore s = history();      return indexesGet(&s);  }
bool adminPostTroeIndexes(void) { IndexStore s = history();      return indexesPost(&s); }
bool adminDeleteTroeIndex(void) { IndexStore s = history();      return indexDelete(&s); }
