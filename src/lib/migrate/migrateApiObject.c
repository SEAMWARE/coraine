//
// FILE            migrateApiObject.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Subscriptions and Context Source Registrations are imported through the SERVICE ROUTINE a client's
// create request reaches - postSubscriptions, postCsourceRegistration - in this process, with no
// request around it. Their validation and every conversion to the stored form (the expanded q, the
// status, the jsonldContext, the registration's own storage shape) is then the broker's current
// code, and nothing here knows the stored format. What a create request cannot set is put right
// afterwards, through the DB driver: modifiedAt, and a subscription's notification counters.
//
// createdAt comes out right by itself: the routine stamps "now", and "now" is the request clock,
// which is set to the source's createdAt for the call.
//
#include <string.h>                                       // strcmp
#include <stdint.h>                                       // int64_t

#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corTree/corTreeBuilder.h"                       // corTreeObject, corTreeInteger, corTreeChildAdd
#include "corRest/CorRestState.h"                         // corRest
#include "corJsonld/corLdExpandTree.h"                    // corLdExpandTree
#include "corJsonld/corLdInit.h"                          // corLdCoreContext

#include "corNgsild/CorNgsild.h"                          // corNgsild
#include "corNgsild/LdVocab.h"                            // LD_VOCAB_*
#include "corNgsild/ldTermId.h"                           // CorTerm*
#include "corNgsild/ldSysTimestamp.h"                     // ldSysTimestampModify

#include "db/DbDriver.h"                                  // db, DB_OK
#include "db/Tenant.h"                                    // Tenant

#if COR_FEATURE_SUBSCRIPTIONS
#include "serviceRoutines/postSubscriptions.h"            // postSubscriptions
#endif
#if COR_FEATURE_REGISTRATIONS
#include "serviceRoutines/postCsourceRegistration.h"      // postCsourceRegistration
#endif

#include "migrate/MigrateState.h"                         // MigrateState
#include "migrate/migrateUtil.h"                          // migrateTermCheck, migrateTimeTake, migrateProblem
#include "migrate/migrateApiObject.h"                     // Own interface



// -----------------------------------------------------------------------------
//
// stringsTermsCheck - an array of names (watchedAttributes, propertyNames, ...)
//
static bool stringsTermsCheck(MigrateState* msP, MigrateKind kind, CorNode* arrayP, const char* what)
{
  if ((arrayP == NULL) || (arrayP->type != CorArray))
    return true;

  for (CorNode* nameP = arrayP->value.head; nameP != NULL; nameP = nameP->next)
  {
    if ((nameP->type == CorString) && (migrateTermCheck(msP, kind, nameP->value.s, what) == false))
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// selectorsTermsCheck - the types of an array of EntitySelectors / EntityInfos
//
// A type that is a type expression (§ 4.17: "(A|B)", "A;B") is left alone: its names are checked
// by its own parser when the broker stores it.
//
static bool selectorsTermsCheck(MigrateState* msP, MigrateKind kind, CorNode* arrayP)
{
  if ((arrayP == NULL) || (arrayP->type != CorArray))
    return true;

  for (CorNode* selP = arrayP->value.head; selP != NULL; selP = selP->next)
  {
    CorNode* typeP = (selP->type == CorObject) ? corTreeLookup(selP, "type") : NULL;

    if ((typeP == NULL) || (typeP->type != CorString))
      continue;

    if (strpbrk(typeP->value.s, "()|;,*") != NULL)
      continue;

    if (migrateTermCheck(msP, kind, typeP->value.s, "entity type") == false)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// viaServiceRoutine - run a create service routine on a tree, as if a request had brought it
//
// Returns the HTTP status the routine answered with (201 on success); the problem, if any, is
// left for migrateProblem.
//
static int viaServiceRoutine(MigrateState* msP, Tenant* tenantP, CorNode* treeP, bool (*routine)(void))
{
  corRest.in.requestTree      = treeP;
  corRest.out.httpStatusCode  = 200;
  corRest.out.headerV         = corRest.out.headers;
  corRest.out.headerCount     = 0;
  corRest.out.headerSize      = sizeof(corRest.out.headers) / sizeof(corRest.out.headers[0]);
  corRest.out.responseTree    = NULL;

  corNgsild.tenantP           = tenantP;
  corNgsild.tenantName        = tenantP->name;
  corNgsild.contextP          = corLdCoreContext();
  corNgsild.userContextBody   = NULL;
  corNgsild.local             = false;

  routine();

  return corRest.out.httpStatusCode;
}



// -----------------------------------------------------------------------------
//
// migrateSubscription - import one Subscription
//
// The notification counters (timesSent, timesFailed, lastNotification, lastSuccess, lastFailure)
// are members of the stream's notification object, as a GET renders them; status is computed
// by the broker and therefore dropped.
//
bool migrateSubscription(MigrateState* msP, Tenant* tenantP, CorNode* subP)
{
#if COR_FEATURE_SUBSCRIPTIONS
  if ((subP == NULL) || (subP->type != CorObject))
    return migrateFail(msP, MigrateSubscription, "the subscription is no JSON object");

  CorNode* idP = corTreeLookup(subP, "id");
  if ((idP == NULL) || (idP->type != CorString))
    return migrateFail(msP, MigrateSubscription, "the subscription has no id - an import never makes one up");

  const char* subId = idP->value.s;

  if ((selectorsTermsCheck(msP, MigrateSubscription, corTreeLookup(subP, "entities"))                         == false) ||
      (stringsTermsCheck(msP, MigrateSubscription, corTreeLookup(subP, "watchedAttributes"), "watched attribute") == false))
    return false;

  CorNode* notifP = corTreeLookup(subP, "notification");
  if ((notifP != NULL) && (stringsTermsCheck(msP, MigrateSubscription, corTreeLookup(notifP, "attributes"), "notification attribute") == false))
    return false;

  int64_t createdAt  = migrateTimeTake(subP, CorTermCreatedAt,  LD_VOCAB_CREATED_AT);
  int64_t modifiedAt = migrateTimeTake(subP, CorTermModifiedAt, LD_VOCAB_MODIFIED_AT);

  if ((createdAt < 0) || (modifiedAt < 0))
    return migrateFail(msP, MigrateSubscription, "'%s': createdAt/modifiedAt is no DateTime", subId);

  migrateStringTake(subP, "status");

  //
  // The counters - out of the tree before the routine sees it: a client cannot set them
  //
  int     timesSent        = 0;
  int     timesFailed      = 0;
  int64_t lastNotification = 0;
  int64_t lastSuccess      = 0;
  int64_t lastFailure      = 0;

  if ((notifP != NULL) && (notifP->type == CorObject))
  {
    CorNode* nP;

    if ((nP = corTreeLookup(notifP, "timesSent"))   != NULL) { timesSent   = (nP->type == CorInt) ? (int) nP->value.i : 0; corTreeChildRemove(notifP, nP); }
    if ((nP = corTreeLookup(notifP, "timesFailed")) != NULL) { timesFailed = (nP->type == CorInt) ? (int) nP->value.i : 0; corTreeChildRemove(notifP, nP); }

    lastNotification = migrateTimeTake(notifP, CorTermLastNotification, "lastNotification");
    lastSuccess      = migrateTimeTake(notifP, CorTermLastSuccess,      "lastSuccess");
    lastFailure      = migrateTimeTake(notifP, CorTermLastFailure,      "lastFailure");

    migrateStringTake(notifP, "status");
  }

  if (modifiedAt == 0) modifiedAt = (createdAt != 0) ? createdAt : (int64_t) corRest.requestStartTime;
  if (createdAt  == 0) createdAt  = modifiedAt;

  corLdExpandTree(subP, corLdCoreContext(), &corRest.kalloc);

  corRest.requestStartTime = (uint64_t) createdAt;

  if (viaServiceRoutine(msP, tenantP, subP, postSubscriptions) != 201)
    return migrateFail(msP, MigrateSubscription, "'%s': %s", subId, migrateProblem());

  if (modifiedAt != createdAt)
  {
    CorNode* fragP = corTreeObject(corRest.kallocP, NULL);

    corTreeChildAdd(fragP, corTreeInteger(corRest.kallocP, LD_VOCAB_MODIFIED_AT, (long long) modifiedAt));

    if ((db.subscriptionUpdate == NULL) || (db.subscriptionUpdate(tenantP, subId, fragP) != DB_OK))
      return migrateFail(msP, MigrateSubscription, "'%s': stored, but its modifiedAt could not be set", subId);
  }

  if ((timesSent != 0) || (timesFailed != 0) || (lastNotification > 0) || (lastSuccess > 0) || (lastFailure > 0))
  {
    if (db.subscriptionStatsFlush == NULL)
      return migrateFail(msP, MigrateSubscription, "'%s': stored, but this database keeps no notification counters", subId);

    if (db.subscriptionStatsFlush(tenantP, subId, timesSent, timesFailed,
                                  (lastNotification > 0) ? (uint64_t) lastNotification : 0,
                                  (lastSuccess      > 0) ? (uint64_t) lastSuccess      : 0,
                                  (lastFailure      > 0) ? (uint64_t) lastFailure      : 0) != DB_OK)
      return migrateFail(msP, MigrateSubscription, "'%s': stored, but its notification counters could not be set", subId);
  }

  msP->okV[MigrateSubscription] += 1;
  return true;
#else
  (void) tenantP;
  (void) subP;
  return migrateFail(msP, MigrateSubscription, "subscriptions are not in this build (COR_FEATURE_SUBSCRIPTIONS)");
#endif
}



// -----------------------------------------------------------------------------
//
// migrateRegistration - import one Context Source Registration
//
bool migrateRegistration(MigrateState* msP, Tenant* tenantP, CorNode* regP)
{
#if COR_FEATURE_REGISTRATIONS
  if ((regP == NULL) || (regP->type != CorObject))
    return migrateFail(msP, MigrateRegistration, "the registration is no JSON object");

  CorNode* idP = corTreeLookup(regP, "id");
  if ((idP == NULL) || (idP->type != CorString))
    return migrateFail(msP, MigrateRegistration, "the registration has no id - an import never makes one up");

  const char* regId = idP->value.s;

  CorNode* infoP = corTreeLookup(regP, "information");
  if ((infoP != NULL) && (infoP->type == CorArray))
  {
    for (CorNode* riP = infoP->value.head; riP != NULL; riP = riP->next)
    {
      if (riP->type != CorObject)
        continue;

      if ((selectorsTermsCheck(msP, MigrateRegistration, corTreeLookup(riP, "entities"))                              == false) ||
          (stringsTermsCheck(msP, MigrateRegistration, corTreeLookup(riP, "propertyNames"),     "property name")      == false) ||
          (stringsTermsCheck(msP, MigrateRegistration, corTreeLookup(riP, "relationshipNames"), "relationship name")  == false))
        return false;
    }
  }

  int64_t createdAt  = migrateTimeTake(regP, CorTermCreatedAt,  LD_VOCAB_CREATED_AT);
  int64_t modifiedAt = migrateTimeTake(regP, CorTermModifiedAt, LD_VOCAB_MODIFIED_AT);

  if ((createdAt < 0) || (modifiedAt < 0))
    return migrateFail(msP, MigrateRegistration, "'%s': createdAt/modifiedAt is no DateTime", regId);

  migrateStringTake(regP, "status");

  if (modifiedAt == 0) modifiedAt = (createdAt != 0) ? createdAt : (int64_t) corRest.requestStartTime;
  if (createdAt  == 0) createdAt  = modifiedAt;

  corLdExpandTree(regP, corLdCoreContext(), &corRest.kalloc);

  corRest.requestStartTime = (uint64_t) createdAt;

  if (viaServiceRoutine(msP, tenantP, regP, postCsourceRegistration) != 201)
    return migrateFail(msP, MigrateRegistration, "'%s': %s", regId, migrateProblem());

  //
  // A registration update stores the WHOLE registration (the broker owns the merge - see
  // patchCsourceRegistration), so modifiedAt is set the way a PATCH sets it: on the stored tree,
  // with the request clock at the source's modifiedAt.
  //
  if (modifiedAt != createdAt)
  {
    CorNode* storedP = NULL;

    if ((db.registrationRetrieve == NULL) || (db.registrationRetrieve(tenantP, regId, &storedP) != DB_OK) || (storedP == NULL))
      return migrateFail(msP, MigrateRegistration, "'%s': stored, but not found again to set its modifiedAt", regId);

    corRest.requestStartTime = (uint64_t) modifiedAt;
    ldSysTimestampModify(storedP);

    if ((db.registrationUpdate == NULL) || (db.registrationUpdate(tenantP, regId, storedP) != DB_OK))
      return migrateFail(msP, MigrateRegistration, "'%s': stored, but its modifiedAt could not be set", regId);
  }

  msP->okV[MigrateRegistration] += 1;
  return true;
#else
  (void) tenantP;
  (void) regP;
  return migrateFail(msP, MigrateRegistration, "registrations are not in this build (COR_FEATURE_REGISTRATIONS)");
#endif
}
