//
// FILE            migrateUtil.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                        // FILE, fopen, fread, vfprintf
#include <stdarg.h>                                       // va_list
#include <stdlib.h>                                       // malloc
#include <string.h>                                       // strcmp, strchr, strncmp

#include "corAlloc/CorAlloc.h"                            // CorAlloc
#include "corAlloc/corAlloc.h"                            // corAlloc
#include "corAlloc/corAllocBufferInit.h"                  // corAllocBufferInit
#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corTree/corTreeBuilder.h"                       // corTreeChildRemove
#include "corJson/corJsonCreate.h"                        // corJsonCreate
#include "corJson/corJsonParse.h"                         // corJsonParse
#include "corRest/CorRestState.h"                         // corRest
#include "corJsonld/corLdExpand.h"                        // corLdAlreadyExpanded, contextItemLookup
#include "corJsonld/corLdCoreLookup.h"                    // corLdCoreLookup
#include "corJsonld/corLdPrefixExpand.h"                  // corLdPrefixExpand
#include "corJsonld/corLdContextParse.h"                  // corLdContextFromTree
#include "corJsonld/corLdDownload.h"                      // corLdContextFromUrl

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
bool migrateTermCheck(MigrateState* msP, MigrateKind kind, const char* term, const char* what)
{
  if ((term == NULL) || (term[0] == 0))
    return migrateFail(msP, kind, "empty %s", what);

  if (term[0] == '@')                                         return true;   // a JSON-LD keyword
  if (corLdAlreadyExpanded(term) == true)                     return true;   // an IRI
  if (corLdCoreLookup(term) != NULL)                          return true;   // a core term

  if (msP->contextP != NULL)
  {
    if (contextItemLookup(msP->contextP, term) != NULL)       return true;   // the user's term
    if ((strchr(term, ':') != NULL) && (corLdPrefixExpand(msP->contextP, term, &corRest.kalloc) != NULL))
      return true;                                                           // prefix:suffix, prefix defined
  }

  if (msP->contextP == NULL)
    return migrateFail(msP, kind, "%s '%s' is not expanded - give the @context that defines it (--importContext)", what, term);

  return migrateFail(msP, kind, "%s '%s' is not defined by the --importContext", what, term);
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



// -----------------------------------------------------------------------------
//
// migrateContextLoad -
//
// The context lives as long as the import: its own arena, never reset.
//
CorLdContext* migrateContextLoad(const char* ref)
{
  static CorAlloc  contextAlloc;
  static char      contextBuffer[64 * 1024];
  static CorJson   contextJson;

  corAllocBufferInit(&contextAlloc, contextBuffer, sizeof(contextBuffer), 64 * 1024, NULL, "importContext");

  if ((strncmp(ref, "http://", 7) == 0) || (strncmp(ref, "https://", 8) == 0))
    return corLdContextFromUrl(ref, &contextAlloc);

  FILE* fP = fopen(ref, "r");
  if (fP == NULL)
  {
    fprintf(stderr, "--importContext: cannot open '%s'\n", ref);
    return NULL;
  }

  fseek(fP, 0, SEEK_END);
  long size = ftell(fP);
  fseek(fP, 0, SEEK_SET);

  char* text = corAlloc(&contextAlloc, size + 1);
  if ((text == NULL) || (fread(text, 1, size, fP) != (size_t) size))
  {
    fclose(fP);
    fprintf(stderr, "--importContext: cannot read '%s'\n", ref);
    return NULL;
  }
  fclose(fP);
  text[size] = 0;

  CorJson* jsonP = corJsonCreate(&contextJson, &contextAlloc);
  CorNode* docP  = corJsonParse(jsonP, text);

  if ((docP == NULL) || (docP->type != CorObject))
  {
    fprintf(stderr, "--importContext: '%s' is no JSON object\n", ref);
    return NULL;
  }

  CorNode* ctxP = corTreeLookup(docP, "@context");
  if (ctxP == NULL)
  {
    fprintf(stderr, "--importContext: '%s' has no @context member\n", ref);
    return NULL;
  }

  return corLdContextFromTree(ctxP, &contextAlloc, NULL);
}
