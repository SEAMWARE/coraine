//
// FILE            migrateUtil.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                        // fprintf, vfprintf, snprintf
#include <stdarg.h>                                       // va_list
#include <string.h>                                       // strcmp, strncpy

#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corTree/corTreeBuilder.h"                       // corTreeChildRemove
#include "corRest/CorRestState.h"                         // corRest
#include "corJsonld/corLdExpand.h"                        // corLdAlreadyExpanded
#include "corJsonld/corLdCoreLookup.h"                    // corLdCoreLookup

#include "corNgsild/ldTermId.h"                           // ldTermId, CorTerm*
#include "corNgsild/ldIsEntityKeyword.h"                  // ldIsEntityMember
#include "corNgsild/ldCheckDateTime.h"                    // ldIsoToNanoseconds

#include "migrate/MigrateState.h"                         // MigrateState, MigrateKind
#include "migrate/migrateUtil.h"                          // Own interface



// -----------------------------------------------------------------------------
//
// kindName - for the error lines
//
static const char* kindName(MigrateKind kind)
{
  switch (kind)
  {
  case MigrateEntity:           return "entity";
  case MigrateSubscription:     return "subscription";
  case MigrateRegistration:     return "registration";
  case MigrateTemporalEntity:   return "temporalEntity";
  case MigrateTemporalInstance: return "temporalInstance";
  default:                      break;
  }

  return "record";
}



// -----------------------------------------------------------------------------
//
// migrateFail -
//
bool migrateFail(MigrateState* msP, MigrateKind kind, const char* format, ...)
{
  va_list args;

  fprintf(stderr, "%s:%d: %s: ", msP->path, msP->lineNo, kindName(kind));

  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);

  fprintf(stderr, "\n");

  if ((int) kind < (int) MigrateKinds)
    msP->failedV[kind] += 1;

  return false;
}



// -----------------------------------------------------------------------------
//
// migrateTermCheck -
//
// The stream is expanded NGSI-LD whatever its source (doc/migration.md): the import expands nothing.
// A name that is neither an IRI nor a core term is a stream that was not expanded - the record is
// refused, the term named.
//
bool migrateTermCheck(MigrateState* msP, MigrateKind kind, const char* term, const char* what)
{
  if ((term == NULL) || (term[0] == 0))
    return migrateFail(msP, kind, "empty %s", what);

  if (term[0] == '@')                                         return true;   // a JSON-LD keyword
  if (corLdAlreadyExpanded(term) == true)                     return true;   // an IRI
  if (corLdCoreLookup(term) != NULL)                          return true;   // a core term

  return migrateFail(msP, kind, "%s '%s' is not expanded - the stream must hold expanded NGSI-LD (full IRIs, core terms short)", what, term);
}



// -----------------------------------------------------------------------------
//
// typeTermsCheck - a type member: a string or an array of strings
//
static bool typeTermsCheck(MigrateState* msP, MigrateKind kind, CorNode* typeP, const char* what)
{
  if (typeP == NULL)
    return true;

  if (typeP->type == CorString)
    return migrateTermCheck(msP, kind, typeP->value.s, what);

  if (typeP->type == CorArray)
  {
    for (CorNode* tP = typeP->value.head; tP != NULL; tP = tP->next)
    {
      if ((tP->type == CorString) && (migrateTermCheck(msP, kind, tP->value.s, what) == false))
        return false;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// isAttrMember - a member of an Attribute that is no Sub-Attribute (type, value, observedAt, ...)
//
// Before expansion, so by name: a core term is structural, anything else is a Sub-Attribute.
//
static bool isAttrMember(CorNode* nodeP)
{
  if (nodeP->name == NULL)
    return true;

  if (nodeP->name[0] == '@')
    return true;

  return (corLdCoreLookup(nodeP->name) != NULL);
}



// -----------------------------------------------------------------------------
//
// instanceTermsCheck - the Sub-Attribute names of an Attribute instance, and a vocab
//
static bool instanceTermsCheck(MigrateState* msP, MigrateKind kind, CorNode* instP)
{
  if (instP->type != CorObject)
    return true;

  for (CorNode* memberP = instP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((memberP->name != NULL) && (strcmp(memberP->name, "vocab") == 0))
    {
      if (typeTermsCheck(msP, kind, memberP, "vocab") == false)
        return false;
      continue;
    }

    if (isAttrMember(memberP) == true)
      continue;

    if (migrateTermCheck(msP, kind, memberP->name, "sub-attribute name") == false)
      return false;

    if (instanceTermsCheck(msP, kind, memberP) == false)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// migrateEntityTermsCheck -
//
bool migrateEntityTermsCheck(MigrateState* msP, MigrateKind kind, CorNode* entityP)
{
  for (CorNode* memberP = entityP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if (memberP->name == NULL)
      continue;

    if ((strcmp(memberP->name, "type") == 0) || (strcmp(memberP->name, "@type") == 0))
    {
      if (typeTermsCheck(msP, kind, memberP, "entity type") == false)
        return false;
      continue;
    }

    if (ldIsEntityKeyword(memberP->name) == true)
      continue;

    if (migrateTermCheck(msP, kind, memberP->name, "attribute name") == false)
      return false;

    if (memberP->type == CorObject)
    {
      if (instanceTermsCheck(msP, kind, memberP) == false)
        return false;
    }
    else if (memberP->type == CorArray)
    {
      for (CorNode* instP = memberP->value.head; instP != NULL; instP = instP->next)
      {
        if (instanceTermsCheck(msP, kind, instP) == false)
          return false;
      }
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// migrateTimeOf -
//
int64_t migrateTimeOf(CorNode* nodeP)
{
  if (nodeP == NULL)
    return 0;

  if (nodeP->type == CorInt)
    return (int64_t) nodeP->value.i;

  if (nodeP->type == CorString)
    return ldIsoToNanoseconds(nodeP->value.s);

  if (nodeP->type == CorObject)
  {
    CorNode* valueP = corTreeLookup(nodeP, "@value");

    if ((valueP != NULL) && (valueP->type == CorString))
      return ldIsoToNanoseconds(valueP->value.s);
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// migrateTimeTake -
//
int64_t migrateTimeTake(CorNode* objP, CorTerm term, const char* name)
{
  if ((objP == NULL) || (objP->type != CorObject))
    return 0;

  for (CorNode* memberP = objP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if ((ldTermId(memberP) != term) && ((memberP->name == NULL) || (strcmp(memberP->name, name) != 0)))
      continue;

    int64_t ns = migrateTimeOf(memberP);

    corTreeChildRemove(objP, memberP);
    return (ns > 0) ? ns : -1;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// migrateStringTake -
//
const char* migrateStringTake(CorNode* objP, const char* name)
{
  CorNode* memberP = ((objP != NULL) && (objP->type == CorObject)) ? corTreeLookup(objP, name) : NULL;

  if (memberP == NULL)
    return NULL;

  corTreeChildRemove(objP, memberP);

  return (memberP->type == CorString) ? memberP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// migrateProblem -
//
const char* migrateProblem(void)
{
  static char detail[sizeof(corRest.out.problemDetail)];

  if (corRest.out.problemDetail[0] != 0)
    strncpy(detail, corRest.out.problemDetail, sizeof(detail) - 1);
  else if (corRest.out.problemTitle != NULL)
    strncpy(detail, corRest.out.problemTitle, sizeof(detail) - 1);
  else
    snprintf(detail, sizeof(detail), "refused (HTTP status %d)", corRest.out.httpStatusCode);

  detail[sizeof(detail) - 1] = 0;

  corRest.out.httpStatusCode   = 0;
  corRest.out.problemType      = NULL;
  corRest.out.problemTitle     = NULL;
  corRest.out.problemDetail[0] = 0;
  corRest.out.problemExtras    = NULL;

  return detail;
}
