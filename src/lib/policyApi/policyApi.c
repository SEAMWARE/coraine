//
// FILE            policyApi.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The ODRL-PAP's /policy API (odrl-pap 1.7.1, api/odrl.yaml), on a listener of its own (corRestListen):
//
//   GET    /policy?page&pageSize    the policies, in the order they were created - [{ id, odrl:uid, odrl }]
//   POST   /policy                  a new one: 200, Location: its id (ten letters, as the PAP's)
//   PUT    /policy/{id}             create or overwrite, with that id
//   GET    /policy/{id}             one - 404
//   DELETE /policy/{id}             204, whether it was there or not (as the PAP)
//   GET    /policy/odrl/{uid}       one, by its odrl:uid - 404
//   DELETE /policy/odrl/{uid}       204, whether it was there or not
//
// A policy is a JSON object with a string "odrl:uid", one uid to a policy. The PAP answers 500 for most
// of what it refuses (a body that is no JSON, no odrl:uid, a uid in use - and any overwrite); this API
// answers 400 / 409 with a ProblemDetails. "odrl" is the policy as a string, as the PAP's: the policy as
// it was given - the PAP's own rewrite (its @context, an array of one made an object) is not made. No
// "rego": there is none. A create's body is empty: the PAP's is the Rego it made.
//
// Each policy is a document of the default tenant's "odrlPolicies": { id, uid, seq, odrl }; seq keeps the
// order of creation, which the PAP's listing has and fdsc.so decides by (the first policy that allows).
//
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // atoi, qsort
#include <stdint.h>                                   // uint64_t
#include <string.h>                                   // strcmp, strlen
#include <pthread.h>                                  // pthread_mutex_t
#include <sys/random.h>                               // getrandom

#include "corLog/corLog.h"                            // COR_I, COR_E
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeArray, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corRest/CorRestState.h"                     // corRest
#include "corRest/CorRestService.h"                   // CorRestServiceSimplified
#include "corRest/corRestInit.h"                      // corRestListen
#include "corRest/corRestProblem.h"                   // corRestProblem
#include "corRest/corRestOutHeader.h"                 // corRestOutHeaderAdd

#include "db/DbDriver.h"                              // db, DB_OK, DB_NOT_FOUND
#include "db/Tenant.h"                                // tenant0
#include "policyApi/policyApi.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// The collection, and the lock of every write: a uid is checked free and taken under it
//
static const char*      collection = "odrlPolicies";
static pthread_mutex_t  writeMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// problem - a ProblemDetails, about:blank
//
static bool problem(int status, const char* title, const char* detail)
{
  corRestProblem(status, "about:blank", title, "%s", detail);
  return true;
}



// -----------------------------------------------------------------------------
//
// strOf - a document's string member, NULL if it has none
//
static const char* strOf(CorNode* docP, const char* name)
{
  CorNode* nodeP = (docP != NULL) ? corTreeLookup(docP, name) : NULL;

  return ((nodeP != NULL) && (nodeP->type == CorString)) ? nodeP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// seqOf - a document's seq, 0 if it has none
//
static long long seqOf(CorNode* docP)
{
  CorNode* nodeP = corTreeLookup(docP, "seq");

  return ((nodeP != NULL) && (nodeP->type == CorInt)) ? nodeP->value.i : 0;
}



// -----------------------------------------------------------------------------
//
// all - every policy, in the order of creation; an array of n documents (vP, in corRest.kalloc)
//
static int seqCompare(const void* a, const void* b)
{
  long long sa = seqOf(*(CorNode**) a);
  long long sb = seqOf(*(CorNode**) b);

  return (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
}

static CorNode** all(int* nP)
{
  CorNode* arrayP = NULL;

  *nP = 0;

  if ((db.docQuery == NULL) || (db.docQuery(&tenant0, collection, &arrayP) != DB_OK) || (arrayP == NULL))
    return NULL;

  int n = 0;

  for (CorNode* docP = arrayP->value.head; docP != NULL; docP = docP->next)
    ++n;

  CorNode** vP = (CorNode**) corAlloc(&corRest.kalloc, (n + 1) * sizeof(CorNode*));

  if (vP == NULL)
    return NULL;

  int ix = 0;

  for (CorNode* docP = arrayP->value.head; docP != NULL; docP = docP->next)
    vP[ix++] = docP;

  qsort(vP, n, sizeof(CorNode*), seqCompare);

  *nP = n;
  return vP;
}



// -----------------------------------------------------------------------------
//
// byUid - the document of the policy with this odrl:uid, NULL if none
//
static CorNode* byUid(const char* uid)
{
  int       n    = 0;
  CorNode** docV = all(&n);

  for (int ix = 0; ix < n; ix++)
  {
    const char* docUid = strOf(docV[ix], "uid");

    if ((docUid != NULL) && (strcmp(docUid, uid) == 0))
      return docV[ix];
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// byId - the document of the policy with this id, NULL if none
//
static CorNode* byId(const char* id)
{
  CorNode* docP = NULL;

  if ((db.docRetrieve == NULL) || (db.docRetrieve(&tenant0, collection, id, &docP) != DB_OK))
    return NULL;

  return docP;
}



// -----------------------------------------------------------------------------
//
// rendered - a policy as the PAP gives it: { "id", "odrl:uid", "odrl": "<the policy as a string>" }
//
static CorNode* rendered(CorNode* docP)
{
  CorNode*    outP  = corTreeObject(corRest.kallocP, NULL);
  CorNode*    odrlP = corTreeLookup(docP, "odrl");
  const char* text  = "{}";

  if (odrlP != NULL)
  {
    int   size = corJsonFastRenderSize(odrlP) + 1;
    char* buf  = (char*) corAlloc(&corRest.kalloc, size);

    if (buf != NULL)
    {
      corJsonFastRender(odrlP, buf);
      text = buf;
    }
  }

  corTreeChildAdd(outP, corTreeString(corRest.kallocP, "id",       strOf(docP, "id")));
  corTreeChildAdd(outP, corTreeString(corRest.kallocP, "odrl:uid", strOf(docP, "uid")));
  corTreeChildAdd(outP, corTreeString(corRest.kallocP, "odrl",     text));

  return outP;
}



// -----------------------------------------------------------------------------
//
// argumentMissing - a constraint whose leftOperand is a word that takes an argument (http:bodyValue,
// ovc:leftOperand: a path), named without it - "odrl:leftOperand": "http:bodyValue" instead of
// "http:bodyValue": "$.a.b". The PAP refuses such a policy since 1.7.2 (SEAMWARE/odrl-pap#63, for #58): its
// Rego could not parse, and OPA then loaded no policy at all. The word, or NULL
//
static const char* argumentMissing(CorNode* nodeP)
{
  if ((nodeP == NULL) || ((nodeP->type != CorObject) && (nodeP->type != CorArray)))
    return NULL;

  if (nodeP->type == CorObject)
  {
    CorNode* leftP = corTreeLookup(nodeP, "odrl:leftOperand");

    if ((leftP != NULL) && (leftP->type == CorObject))
      leftP = corTreeLookup(leftP, "@id");

    if ((leftP != NULL) && (leftP->type == CorString) &&
        ((strcmp(leftP->value.s, "http:bodyValue") == 0) || (strcmp(leftP->value.s, "ovc:leftOperand") == 0)))
      return leftP->value.s;
  }

  for (CorNode* childP = nodeP->value.head; childP != NULL; childP = childP->next)
  {
    const char* word = argumentMissing(childP);

    if (word != NULL)
      return word;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// bodyUid - the body's odrl:uid; NULL: the 400 is set
//
static const char* bodyUid(void)
{
  CorNode* bodyP = corRest.in.requestTree;

  if ((bodyP == NULL) || (bodyP->type != CorObject))
  {
    problem(400, "Bad Request", "the body must be a JSON object - an ODRL policy");
    return NULL;
  }

  const char* uid = strOf(bodyP, "odrl:uid");

  if ((uid == NULL) || (*uid == 0))
  {
    problem(400, "Bad Request", "the policy has no 'odrl:uid' (a string)");
    return NULL;
  }

  //
  // The PAP's own words (MappingException); it answers them as a 500 (its IllegalArgumentException handler
  // does not get to say 400) - a 400 here, the policy refused as there
  //
  const char* word = argumentMissing(bodyP);

  if (word != NULL)
  {
    char detail[256];

    snprintf(detail, sizeof(detail), "The leftOperand `%s` requires 1 parameter(s), but the policy does not provide a value. Provide it as `\"%s\": \"<value>\"`.", word, word);
    problem(400, "Bad Request", detail);
    return NULL;
  }

  return uid;
}



// -----------------------------------------------------------------------------
//
// idMake - ten lowercase letters, as the PAP's ids
//
static char* idMake(void)
{
  unsigned char bytes[10];
  char*         id = (char*) corAlloc(&corRest.kalloc, sizeof(bytes) + 1);

  if ((id == NULL) || (getrandom(bytes, sizeof(bytes), 0) != (ssize_t) sizeof(bytes)))
    return NULL;

  for (unsigned int ix = 0; ix < sizeof(bytes); ix++)
    id[ix] = 'a' + (bytes[ix] % 26);
  id[sizeof(bytes)] = 0;

  return id;
}



// -----------------------------------------------------------------------------
//
// store - the body as policy 'id' (replacing it if 'existing'), under writeMutex; the response set
//
static bool store(const char* id, const char* uid, CorNode* existingP)
{
  CorNode* takenP = byUid(uid);

  if ((takenP != NULL) && (strcmp(strOf(takenP, "id"), id) != 0))
  {
    corRestProblem(409, "about:blank", "Conflict", "'odrl:uid' %s is policy %s's", uid, strOf(takenP, "id"));
    return true;
  }

  long long seq = (existingP != NULL) ? seqOf(existingP) : 0;

  if (existingP == NULL)
  {
    int       n    = 0;
    CorNode** docV = all(&n);

    seq = (n > 0) ? seqOf(docV[n - 1]) + 1 : 1;
  }

  CorNode* docP = corTreeObject(corRest.kallocP, NULL);
  CorNode* odrlP = corRest.in.requestTree;

  odrlP->name = (char*) "odrl";
  corTreeChildAdd(docP, corTreeString(corRest.kallocP,  "id",  id));
  corTreeChildAdd(docP, corTreeString(corRest.kallocP,  "uid", uid));
  corTreeChildAdd(docP, corTreeInteger(corRest.kallocP, "seq", seq));
  corTreeChildAdd(docP, odrlP);
  corRest.in.requestTree = NULL;                     // the document's now

  int rc = (existingP != NULL) ? db.docReplace(&tenant0, collection, id, docP) : db.docCreate(&tenant0, collection, id, docP);

  if (rc != DB_OK)
  {
    COR_E("policy %s (%s): the store refused it (%d)", id, uid, rc);
    return problem(500, "Internal Server Error", "the policy could not be stored");
  }

  COR_I("policy %s (%s) %s", id, uid, (existingP != NULL) ? "replaced" : "created");

  corRestOutHeaderAdd("Location", id);
  corRest.out.httpStatusCode = 200;
  corRest.out.contentType    = (char*) "text/plain";
  corRest.out.payload        = (char*) "";
  corRest.out.payloadSize    = 0;

  return true;
}



// -----------------------------------------------------------------------------
//
// policyList - GET /policy
//
static bool policyList(void)
{
  int page     = 0;
  int pageSize = 25;

  for (int ix = 0; ix < corRest.in.uriParamCount; ix++)
  {
    const char* key   = corRest.in.uriParamV[ix].key;
    const char* value = corRest.in.uriParamV[ix].value;

    if (key == NULL)
      continue;
    else if (strcmp(key, "page") == 0)
      page = atoi(value);
    else if (strcmp(key, "pageSize") == 0)
      pageSize = atoi(value);
  }

  if ((page < 0) || (pageSize < 1))
    return problem(400, "Bad Request", "'page' must be 0 or more, 'pageSize' 1 or more");

  int       n      = 0;
  CorNode** docV   = all(&n);
  CorNode*  arrayP = corTreeArray(corRest.kallocP, NULL);
  long long first  = (long long) page * pageSize;

  for (long long ix = first; (ix < n) && (ix < first + pageSize); ix++)
    corTreeChildAdd(arrayP, rendered(docV[ix]));

  corRest.out.responseTree   = arrayP;
  corRest.out.httpStatusCode = 200;
  return true;
}



// -----------------------------------------------------------------------------
//
// policyCreate - POST /policy
//
static bool policyCreate(void)
{
  const char* uid = bodyUid();

  if (uid == NULL)
    return true;

  pthread_mutex_lock(&writeMutex);

  char* id = idMake();

  while ((id != NULL) && (byId(id) != NULL))
    id = idMake();

  if (id == NULL)
    problem(500, "Internal Server Error", "no id could be made");
  else
    store(id, uid, NULL);

  pthread_mutex_unlock(&writeMutex);
  return true;
}



// -----------------------------------------------------------------------------
//
// policyPut - PUT /policy/{id}
//
static bool policyPut(void)
{
  const char* id  = corRest.in.wildcard[0];
  const char* uid = bodyUid();

  if (uid == NULL)
    return true;

  pthread_mutex_lock(&writeMutex);
  store(id, uid, byId(id));
  pthread_mutex_unlock(&writeMutex);

  return true;
}



// -----------------------------------------------------------------------------
//
// policyGet - GET /policy/{id}
//
static bool policyGet(void)
{
  CorNode* docP = byId(corRest.in.wildcard[0]);

  if (docP == NULL)
    return problem(404, "Not Found", "no such policy");

  corRest.out.responseTree   = rendered(docP);
  corRest.out.httpStatusCode = 200;
  return true;
}



// -----------------------------------------------------------------------------
//
// policyDelete - DELETE /policy/{id}
//
static bool policyDelete(void)
{
  const char* id = corRest.in.wildcard[0];

  pthread_mutex_lock(&writeMutex);
  if (db.docDelete(&tenant0, collection, id) == DB_OK)
    COR_I("policy %s deleted", id);
  pthread_mutex_unlock(&writeMutex);

  corRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// policyGetByUid - GET /policy/odrl/{uid}
//
static bool policyGetByUid(void)
{
  CorNode* docP = byUid(corRest.in.wildcard[0]);

  if (docP == NULL)
    return problem(404, "Not Found", "no such policy");

  corRest.out.responseTree   = rendered(docP);
  corRest.out.httpStatusCode = 200;
  return true;
}



// -----------------------------------------------------------------------------
//
// policyDeleteByUid - DELETE /policy/odrl/{uid}
//
static bool policyDeleteByUid(void)
{
  pthread_mutex_lock(&writeMutex);

  CorNode* docP = byUid(corRest.in.wildcard[0]);

  if ((docP != NULL) && (db.docDelete(&tenant0, collection, strOf(docP, "id")) == DB_OK))
    COR_I("policy %s (%s) deleted", strOf(docP, "id"), corRest.in.wildcard[0]);

  pthread_mutex_unlock(&writeMutex);

  corRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// The routes - the more specific first: the router matches in registration order
//
static CorRestServiceSimplified routeV[] =
{
  { CorVerbGet,    "/policy/odrl/*", policyGetByUid,    0,               0 },
  { CorVerbDelete, "/policy/odrl/*", policyDeleteByUid, 0,               0 },
  { CorVerbGet,    "/policy/*",      policyGet,         0,               0 },
  { CorVerbPut,    "/policy/*",      policyPut,         0,               0, NULL, { .features.plainJsonBody = 1 } },
  { CorVerbDelete, "/policy/*",      policyDelete,      0,               0 },
  { CorVerbGet,    "/policy",        policyList,        ~(uint64_t) 0,   0 },   // page, pageSize - and the PAP ignores the rest
  { CorVerbPost,   "/policy",        policyCreate,      0,               0, NULL, { .features.plainJsonBody = 1 } }
};



// -----------------------------------------------------------------------------
//
// policyApiStart -
//
bool policyApiStart(const char* host, unsigned short port)
{
  if ((db.docCreate == NULL) || (db.docQuery == NULL))
  {
    COR_E("--policyApiPort: the store has no documents (DbDriver.doc*)");
    return false;
  }

  if (corRestListen(host, port, routeV, sizeof(routeV) / sizeof(routeV[0])) != 0)
    return false;

  COR_I("the policy API (/policy) on %s port %u", host, port);
  return true;
}
