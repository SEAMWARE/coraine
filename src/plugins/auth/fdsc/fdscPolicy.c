//
// FILE            fdscPolicy.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The ODRL-PAP's vocabulary (its mapping.json) and the Rego each word is mapped to, reimplemented on the
// request the broker hands over - the reference is odrl-pap 1.7.1 as the Data Space Connector 10.87.0
// deploys it (doc/authorization.md § 5 lists what each word reads).
//
// Evaluation is three-valued, as Rego's is: true, false, and UNDEFINED - a value the request or the
// credential does not have. A rule is only satisfied by true; a constraint on an undefined value is
// undefined, and so is every logical constraint that holds one.
//
#define _GNU_SOURCE                                  // strptime, timegm, strtok_r

#include <pthread.h>                                 // pthread_rwlock_*
#include <regex.h>                                   // regcomp, regexec
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // calloc, free, strtoll, strtod
#include <string.h>                                  // strcmp, strncmp, strlen, strchr, memcpy
#include <time.h>                                    // time, gmtime_r, timegm, clock_gettime

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAlloc.h"                       // corAlloc, corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"             // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"            // corAllocBufferReset
#include "corJson/CorJson.h"                         // CorJson
#include "corJson/corJsonCreate.h"                   // corJsonCreate
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corLog/corLog.h"                           // COR_W
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeBuilder.h"                  // corTreeString, corTreeInteger, corTreeArray, corTreeBoolean, corTreeChildAdd
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "corNgsild/LdOp.h"                          // LdOp*

#include "fdscPolicy.h"                              // Own interface



// -----------------------------------------------------------------------------
//
// Tri - a rule's value
//
typedef enum Tri { TriFalse = 0, TriTrue = 1, TriUndef = 2, TriPending = 3 } Tri;   // Pending: --fdscNgsild - it depends on the entity, entity() decides



// -----------------------------------------------------------------------------
//
// HTTP methods, as the actions name them
//
#define M_GET     (1 << 0)
#define M_POST    (1 << 1)
#define M_PUT     (1 << 2)
#define M_PATCH   (1 << 3)
#define M_DELETE  (1 << 4)



// -----------------------------------------------------------------------------
//
// The words
//
typedef enum RuleKind
{
  RuleAnd,                                           // odrl:and, odrl:andSequence, a list of constraints
  RuleOr,                                            // odrl:or
  RuleXone,                                          // odrl:xone
  RuleAction,                                        // the method is one of 'methods'
  RuleTrue,                                          // vc:any, json:any, dspace:use, ovc:constraint
  RuleUndef,                                         // a word that reads what an HTTP request does not have (json:*, dspace:read)
  RuleIssuer,                                        // a string assignee: the credential's issuer is it
  RuleTarget,                                        // a target uid: the last path segment is it
  RuleConstraint
} RuleKind;

typedef enum LeftKind
{
  LeftEntityType,                                    // ngsi-ld:entityType, ngsi-ld:entityTypeGroup
  LeftVcRole,                                        // vc:role
  LeftVcType,                                        // vc:type
  LeftVcIssuer,                                      // vc:currentParty, dome-op:currentParty
  LeftHttpPath,                                      // http:path
  LeftBodyValue,                                     // http:bodyValue "$.a.b"
  LeftDateTime,                                      // odrl:dateTime
  LeftDayOfWeek,                                     // odrl:dayOfWeek
  LeftHourOfDay,                                     // odrl:hourOfDay
  LeftVcClaim,                                       // ovc:leftOperand "a.b"
  LeftCredentialSubjectType,                         // ovc:credentialSubjectType (an extra key of a constraint)
  LeftDomeRole,                                      // dome-op:role
  LeftTmfLifecycle,                                  // tmf:lifecycleStatus
  LeftTmfResource,                                   // tmf:resource
  LeftLiteral,                                       // a word the PAP does not map: the word itself
  LeftUndef                                          // a word that reads what an HTTP request does not have, or makes a call of its own (dome-op:relatedParty)
} LeftKind;

typedef enum OpKind
{
  OpEq, OpNeq, OpGt, OpGteq, OpLt, OpLteq, OpHasPart, OpIsPartOf, OpIsAnyOf, OpIsNoneOf, OpIsAllOf, OpRegex, OpIsInPath
} OpKind;

typedef enum RightKind
{
  RightValue,                                        // a value
  RightNow,                                          // odrl:policyUsage - now, in ms
  RightIssuer                                        // dome-op:currentParty - the credential's issuer
} RightKind;

typedef struct Rule
{
  RuleKind      kind;
  struct Rule*  childV;
  int           children;

  int           methods;                             // RuleAction
  const char*   text;                                // RuleIssuer, RuleTarget

  LeftKind      left;                                // RuleConstraint
  const char*   leftArg;                             // the path of http:bodyValue / ovc:leftOperand; the word of LeftLiteral
  OpKind        op;
  RightKind     right;
  CorNode*      rightP;
  regex_t*      regexP;                              // OpRegex, compiled once
} Rule;

typedef struct Policy
{
  const char*  uid;
  Rule         all;                                  // RuleAnd over every rule of every permission
} Policy;

typedef struct PolicySet
{
  CorAlloc     ka;
  char         buf[16 * 1024];
  Policy*      policyV;
  int          policies;
  int          policySize;
  regex_t**    regexV;                               // to regfree
  int          regexes;
  int          regexSize;
} PolicySet;

static pthread_rwlock_t  setLock       = PTHREAD_RWLOCK_INITIALIZER;
static PolicySet*        setP          = NULL;       // in use
static PolicySet*        buildP        = NULL;       // being built (the refresh thread's)
static bool              loaded        = false;
static char              organization[256] = "urn:ngsi-ld:organization:0b03975e-7ded-4fbd-9c3b-a5d6550df7e2";   // the PAP's default



// =============================================================================
//
// Reading a policy
//
// =============================================================================



// -----------------------------------------------------------------------------
//
// norm - a name as the PAP's compacted form spells it: the ODRL namespace as "odrl:"
//
static const char* norm(const char* s)
{
  static const char odrlNs[] = "http://www.w3.org/ns/odrl/2/";

  if ((s != NULL) && (strncmp(s, odrlNs, sizeof(odrlNs) - 1) == 0))
  {
    static __thread char buf[256];
    snprintf(buf, sizeof(buf), "odrl:%s", &s[sizeof(odrlNs) - 1]);
    return buf;
  }

  return s;
}



// -----------------------------------------------------------------------------
//
// odrl - a member of an object by its ODRL name ("permission"): odrl:permission, or the full IRI
//
static CorNode* odrl(CorNode* objP, const char* name)
{
  if ((objP == NULL) || (objP->type != CorObject))
    return NULL;

  char key[128];

  snprintf(key, sizeof(key), "odrl:%s", name);
  CorNode* nodeP = corTreeLookup(objP, key);

  if (nodeP == NULL)
  {
    snprintf(key, sizeof(key), "http://www.w3.org/ns/odrl/2/%s", name);
    nodeP = corTreeLookup(objP, key);
  }

  return nodeP;
}



// -----------------------------------------------------------------------------
//
// idOf - a name: a string, or an object's @id (or @value)
//
static const char* idOf(CorNode* nodeP)
{
  if (nodeP == NULL)
    return NULL;

  if (nodeP->type == CorString)
    return nodeP->value.s;

  if (nodeP->type == CorObject)
  {
    CorNode* idP = corTreeLookup(nodeP, "@id");

    if (idP == NULL)
      idP = corTreeLookup(nodeP, "@value");

    if ((idP != NULL) && (idP->type == CorString))
      return idP->value.s;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// typeIs - the object's @type is 'type' (odrl: or full IRI)
//
static bool typeIs(CorNode* nodeP, const char* type)
{
  if ((nodeP == NULL) || (nodeP->type != CorObject))
    return false;

  CorNode* tP = corTreeLookup(nodeP, "@type");

  return (tP != NULL) && (tP->type == CorString) && (strcmp(norm(tP->value.s), type) == 0);
}



// -----------------------------------------------------------------------------
//
// listOf - a list: an array, an {"@list": [...]}, or one element (as one-element list) - its first node
//
static CorNode* listOf(CorNode* nodeP, bool* singleP)
{
  *singleP = false;

  if (nodeP == NULL)
    return NULL;

  if (nodeP->type == CorArray)
    return nodeP->value.head;

  if (nodeP->type == CorObject)
  {
    CorNode* listP = corTreeLookup(nodeP, "@list");

    if ((listP != NULL) && (listP->type == CorArray))
      return listP->value.head;
  }

  *singleP = true;
  return nodeP;
}



// -----------------------------------------------------------------------------
//
// Rule building - children are collected in a growable array in the set's arena
//
typedef struct RuleList
{
  Rule*  v;
  int    n;
  int    size;
} RuleList;

static Rule* ruleAdd(PolicySet* psP, RuleList* listP)
{
  if (listP->n == listP->size)
  {
    int   size = (listP->size == 0) ? 4 : listP->size * 2;
    Rule* v    = (Rule*) corAlloc(&psP->ka, size * sizeof(Rule));

    if (v == NULL)
      return NULL;

    if (listP->n > 0)
      memcpy(v, listP->v, listP->n * sizeof(Rule));

    listP->v    = v;
    listP->size = size;
  }

  Rule* rP = &listP->v[listP->n++];
  memset(rP, 0, sizeof(Rule));
  return rP;
}

static void ruleSet(Rule* rP, RuleKind kind, RuleList* childrenP)
{
  rP->kind     = kind;
  rP->childV   = childrenP->v;
  rP->children = childrenP->n;
}



static bool constraintParse(PolicySet* psP, CorNode* cP, Rule* rP, char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// logicalParse - an odrl:LogicalConstraint (and / andSequence / or / xone); false: not one
//
static bool logicalParse(PolicySet* psP, CorNode* cP, Rule* rP, bool* okP, char* errorBuf, int errorBufSize)
{
  static const struct { const char* name; RuleKind kind; } opV[] =
  {
    { "and", RuleAnd }, { "andSequence", RuleAnd }, { "or", RuleOr }, { "xone", RuleXone }, { NULL, RuleAnd }
  };

  for (int i = 0; opV[i].name != NULL; i++)
  {
    CorNode* listP = odrl(cP, opV[i].name);

    if (listP == NULL)
      continue;

    RuleList  children = { NULL, 0, 0 };
    bool      single;

    *okP = true;

    for (CorNode* eP = listOf(listP, &single); eP != NULL; eP = (single == true) ? NULL : eP->next)
    {
      Rule* childP = ruleAdd(psP, &children);

      if ((childP == NULL) || (constraintParse(psP, eP, childP, errorBuf, errorBufSize) == false))
      {
        *okP = false;
        return true;
      }
    }

    ruleSet(rP, opV[i].kind, &children);
    return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// leftOf - a leftOperand's word
//
static LeftKind leftOf(const char* name)
{
  static const struct { const char* name; LeftKind kind; } leftV[] =
  {
    { "ngsi-ld:entityType",       LeftEntityType   },
    { "ngsi-ld:entityTypeGroup",  LeftEntityType   },
    { "vc:role",                  LeftVcRole       },
    { "vc:type",                  LeftVcType       },
    { "vc:currentParty",          LeftVcIssuer     },
    { "dome-op:currentParty",     LeftVcIssuer     },
    { "http:path",                LeftHttpPath     },
    { "odrl:dateTime",            LeftDateTime     },
    { "odrl:dayOfWeek",           LeftDayOfWeek    },
    { "odrl:hourOfDay",           LeftHourOfDay    },
    { "dome-op:role",             LeftDomeRole     },
    { "tmf:lifecycleStatus",      LeftTmfLifecycle },
    { "tmf:resource",             LeftTmfResource  },
    { NULL,                       LeftLiteral      }
  };

  for (int i = 0; leftV[i].name != NULL; i++)
  {
    if (strcmp(leftV[i].name, name) == 0)
      return leftV[i].kind;
  }

  //
  // Words of the other PEPs (json:, dspace:) and the dome-op words that read the stored entity through a
  // call of the Rego's own: nothing an HTTP request here has
  //
  if ((strncmp(name, "json:", 5) == 0) || (strncmp(name, "dspace:", 7) == 0) || (strncmp(name, "dome-op:", 8) == 0))
    return LeftUndef;

  return LeftLiteral;
}



// -----------------------------------------------------------------------------
//
// opOf - an operator's word; -1: not one
//
static int opOf(const char* name)
{
  static const struct { const char* name; OpKind op; } opV[] =
  {
    { "odrl:eq",       OpEq       }, { "odrl:neq",     OpNeq      },
    { "odrl:gt",       OpGt       }, { "odrl:gteq",    OpGteq     },
    { "odrl:lt",       OpLt       }, { "odrl:lteq",    OpLteq     },
    { "odrl:hasPart",  OpHasPart  }, { "odrl:isPartOf", OpIsPartOf },
    { "odrl:isAnyOf",  OpIsAnyOf  }, { "odrl:anyOf",   OpIsAnyOf  },
    { "odrl:isNoneOf", OpIsNoneOf }, { "odrl:isAllOf", OpIsAllOf  },
    { "odrl:regex",    OpRegex    }, { "http:isInPath", OpIsInPath },
    { NULL,            OpEq       }
  };

  for (int i = 0; opV[i].name != NULL; i++)
  {
    if (strcmp(opV[i].name, name) == 0)
      return opV[i].op;
  }

  return -1;
}



// -----------------------------------------------------------------------------
//
// dateMs - an xsd:date (yyyy-MM-dd) as ms since the epoch, at 00:00 UTC
//
static bool dateMs(const char* text, long long* msP)
{
  struct tm tm;

  memset(&tm, 0, sizeof(tm));

  if (strptime(text, "%Y-%m-%d", &tm) == NULL)
    return false;

  *msP = (long long) timegm(&tm) * 1000;
  return true;
}



// -----------------------------------------------------------------------------
//
// rightParse - a rightOperand, as the PAP's RightOperandMapper turns it into a Rego value
//
static CorNode* rightValue(PolicySet* psP, CorNode* nodeP);

static bool rightParse(PolicySet* psP, CorNode* nodeP, Rule* rP)
{
  const char* name = (nodeP->type == CorString) ? nodeP->value.s : NULL;

  if ((nodeP->type == CorObject) && (corTreeLookup(nodeP, "@value") == NULL))
    name = idOf(nodeP);

  if (name != NULL)
  {
    if (strcmp(norm(name), "odrl:policyUsage") == 0)
    {
      rP->right = RightNow;
      return true;
    }

    if (strcmp(name, "dome-op:currentParty") == 0)
    {
      rP->right = RightIssuer;
      return true;
    }
  }

  rP->right  = RightValue;
  rP->rightP = rightValue(psP, nodeP);

  return rP->rightP != NULL;
}

static CorNode* rightValue(PolicySet* psP, CorNode* nodeP)
{
  if (nodeP->type == CorArray)
  {
    CorNode* arrayP = corTreeArray(&psP->ka, NULL);

    for (CorNode* eP = nodeP->value.head; eP != NULL; eP = eP->next)
    {
      CorNode* vP = rightValue(psP, eP);

      if (vP == NULL)
        return NULL;

      vP->next = NULL;
      corTreeChildAdd(arrayP, vP);
    }

    return arrayP;
  }

  if (nodeP->type != CorObject)
  {
    CorNode* copyP = (CorNode*) corAlloc(&psP->ka, sizeof(CorNode));

    if (copyP != NULL)
    {
      *copyP      = *nodeP;
      copyP->next = NULL;
      copyP->name = NULL;
    }

    return copyP;
  }

  CorNode* valueP = corTreeLookup(nodeP, "@value");

  if (valueP == NULL)
  {
    const char* id = idOf(nodeP);
    return (id != NULL) ? corTreeString(&psP->ka, NULL, id) : NULL;
  }

  CorNode*    typeP = corTreeLookup(nodeP, "@type");
  const char* type  = ((typeP != NULL) && (typeP->type == CorString)) ? typeP->value.s : "";
  const char* text  = (valueP->type == CorString) ? valueP->value.s : NULL;

  if (strncmp(type, "http://www.w3.org/2001/XMLSchema#", 33) == 0)
  {
    static __thread char xsd[64];
    snprintf(xsd, sizeof(xsd), "xsd:%s", &type[33]);
    type = xsd;
  }

  if (strcmp(type, "xsd:date") == 0)
  {
    long long ms;

    if ((text == NULL) || (dateMs(text, &ms) == false))
      return NULL;

    return corTreeInteger(&psP->ka, NULL, ms);
  }

  static const char* integerV[] = { "xsd:integer", "xsd:long", "xsd:int", "xsd:short", "xsd:byte", "xsd:nonNegativeInteger", "xsd:positiveInteger", "xsd:negativeInteger", "xsd:nonPositiveInteger", "xsd:unsignedLong", "xsd:unsignedInt", "xsd:unsignedShort", "xsd:unsignedByte", NULL };
  static const char* decimalV[] = { "xsd:decimal", "xsd:float", "xsd:double", NULL };

  for (int i = 0; integerV[i] != NULL; i++)
  {
    if (strcmp(type, integerV[i]) == 0)
    {
      if (valueP->type == CorInt)    return corTreeInteger(&psP->ka, NULL, valueP->value.i);
      if (valueP->type == CorFloat)  return corTreeInteger(&psP->ka, NULL, (long long) valueP->value.f);
      if (text != NULL)              return corTreeInteger(&psP->ka, NULL, strtoll(text, NULL, 10));
      return NULL;
    }
  }

  for (int i = 0; decimalV[i] != NULL; i++)
  {
    if (strcmp(type, decimalV[i]) == 0)
    {
      if (valueP->type == CorInt)    return corTreeFloat(&psP->ka, NULL, (double) valueP->value.i);
      if (valueP->type == CorFloat)  return corTreeFloat(&psP->ka, NULL, valueP->value.f);
      if (text != NULL)              return corTreeFloat(&psP->ka, NULL, strtod(text, NULL));
      return NULL;
    }
  }

  return rightValue(psP, valueP);                    // any other @type: the value as it is
}



// -----------------------------------------------------------------------------
//
// constraintParse - an odrl:Constraint or odrl:LogicalConstraint
//
static bool constraintParse(PolicySet* psP, CorNode* cP, Rule* rP, char* errorBuf, int errorBufSize)
{
  if ((cP == NULL) || (cP->type != CorObject))
  {
    snprintf(errorBuf, errorBufSize, "a constraint that is not an object");
    return false;
  }

  bool ok;

  if (logicalParse(psP, cP, rP, &ok, errorBuf, errorBufSize) == true)
    return ok;

  Rule      main;
  CorNode*  leftP   = odrl(cP, "leftOperand");
  CorNode*  opP     = odrl(cP, "operator");
  CorNode*  rightP  = odrl(cP, "rightOperand");

  memset(&main, 0, sizeof(main));
  main.kind = RuleConstraint;

  if (leftP != NULL)
  {
    const char* name = idOf(leftP);

    if (name == NULL)
    {
      snprintf(errorBuf, errorBufSize, "a leftOperand that is not a name");
      return false;
    }

    name = norm(name);

    //
    // The words that take an argument, as a leftOperand without one: the PAP maps them to Rego with the
    // argument's '%s' left in it - Rego that does not parse, and OPA then refuses the whole bundle: every
    // request of the deployment denied. Here the policy is skipped, and the others decide.
    //
    if ((strcmp(name, "http:bodyValue") == 0) || (strcmp(name, "ovc:leftOperand") == 0))
    {
      snprintf(errorBuf, errorBufSize, "'%.64s' as a leftOperand: its path is missing (\"%.64s\": \"$.a.b\") - in the Data Space Connector this policy stops OPA from loading any", name, name);
      return false;
    }

    main.left    = leftOf(name);
    main.leftArg = corAllocStrdup(&psP->ka, name);
  }
  else if ((leftP = corTreeLookup(cP, "http:bodyValue")) != NULL)
  {
    main.left    = LeftBodyValue;
    main.leftArg = idOf(leftP);
  }
  else if ((leftP = corTreeLookup(cP, "ovc:leftOperand")) != NULL)
  {
    main.left    = LeftVcClaim;
    main.leftArg = idOf(leftP);
  }

  if ((leftP == NULL) || (opP == NULL) || (rightP == NULL) || (idOf(opP) == NULL))
  {
    snprintf(errorBuf, errorBufSize, "a constraint without leftOperand, operator or rightOperand");
    return false;
  }

  int op = opOf(norm(idOf(opP)));

  if (op < 0)
  {
    snprintf(errorBuf, errorBufSize, "unknown operator '%s'", idOf(opP));
    return false;
  }

  main.op = (OpKind) op;

  if (rightParse(psP, rightP, &main) == false)
  {
    snprintf(errorBuf, errorBufSize, "a rightOperand that cannot be read");
    return false;
  }

  if ((main.op == OpRegex) && (main.right == RightValue) && (main.rightP->type == CorString))
  {
    main.regexP = (regex_t*) calloc(1, sizeof(regex_t));

    if ((main.regexP == NULL) || (regcomp(main.regexP, main.rightP->value.s, REG_EXTENDED | REG_NOSUB) != 0))
    {
      free(main.regexP);
      snprintf(errorBuf, errorBufSize, "a regex that does not compile: '%s'", main.rightP->value.s);
      return false;
    }

    if (psP->regexes == psP->regexSize)
    {
      int        size = (psP->regexSize == 0) ? 4 : psP->regexSize * 2;
      regex_t**  v    = (regex_t**) realloc(psP->regexV, size * sizeof(regex_t*));

      if (v == NULL)
      {
        regfree(main.regexP);
        free(main.regexP);
        return false;
      }

      psP->regexV    = v;
      psP->regexSize = size;
    }

    psP->regexV[psP->regexes++] = main.regexP;
  }

  //
  // An extra key (ovc:credentialSubjectType): the constraint AND it
  //
  CorNode* cstP = corTreeLookup(cP, "ovc:credentialSubjectType");

  if (cstP == NULL)
  {
    *rP = main;
    return true;
  }

  RuleList children = { NULL, 0, 0 };
  Rule*    firstP   = ruleAdd(psP, &children);
  Rule*    extraP   = ruleAdd(psP, &children);

  if ((firstP == NULL) || (extraP == NULL))
    return false;

  *firstP = main;
  extraP->kind   = RuleConstraint;
  extraP->left   = LeftCredentialSubjectType;
  extraP->op     = OpEq;
  extraP->right  = RightValue;
  extraP->rightP = rightValue(psP, cstP);

  ruleSet(rP, RuleAnd, &children);
  return extraP->rightP != NULL;
}



// -----------------------------------------------------------------------------
//
// refinementParse - an odrl:refinement: a list (AND), one constraint, or a logical constraint
//
static bool refinementParse(PolicySet* psP, CorNode* refP, RuleList* rulesP, char* errorBuf, int errorBufSize)
{
  Rule* rP = ruleAdd(psP, rulesP);

  if (rP == NULL)
    return false;

  if (refP->type != CorArray)
    return constraintParse(psP, refP, rP, errorBuf, errorBufSize);

  RuleList children = { NULL, 0, 0 };

  for (CorNode* cP = refP->value.head; cP != NULL; cP = cP->next)
  {
    Rule* childP = ruleAdd(psP, &children);

    if ((childP == NULL) || (constraintParse(psP, cP, childP, errorBuf, errorBufSize) == false))
      return false;
  }

  ruleSet(rP, RuleAnd, &children);
  return true;
}



// -----------------------------------------------------------------------------
//
// collectionParse - an odrl:AssetCollection / odrl:PartyCollection: its refinement - the PAP refuses one
// without ("No refinement contained in the collection.")
//
static bool collectionParse(PolicySet* psP, CorNode* collectionP, RuleList* rulesP, char* errorBuf, int errorBufSize)
{
  CorNode* refP = odrl(collectionP, "refinement");

  if (refP == NULL)
  {
    snprintf(errorBuf, errorBufSize, "no refinement in the collection");
    return false;
  }

  return refinementParse(psP, refP, rulesP, errorBuf, errorBufSize);
}



// -----------------------------------------------------------------------------
//
// actionParse -
//
static bool actionParse(PolicySet* psP, CorNode* actionP, RuleList* rulesP, char* errorBuf, int errorBufSize)
{
  static const struct { const char* name; RuleKind kind; int methods; } actionV[] =
  {
    { "odrl:read",      RuleAction, M_GET    },
    { "odrl:modify",    RuleAction, M_PATCH  },
    { "odrl:delete",    RuleAction, M_DELETE },
    { "odrl:use",       RuleAction, M_GET | M_POST | M_PUT | M_PATCH | M_DELETE },
    { "ngsild:create",  RuleAction, M_POST   },
    { "tmf:create",     RuleAction, M_POST   },
    { "dome-op:modify", RuleAction, M_PATCH  },
    { "dome-op:write",  RuleAction, M_PATCH  },
    { "dome-op:read",   RuleAction, M_GET    },
    { "dome-op:create", RuleAction, M_POST   },
    { "dspace:use",     RuleTrue,   0        },
    { "dspace:read",    RuleUndef,  0        },
    { "json:read",      RuleUndef,  0        },
    { "json:write",     RuleUndef,  0        },
    { "json:create",    RuleUndef,  0        },
    { "json:delete",    RuleUndef,  0        },
    { "json:use",       RuleUndef,  0        },
    { NULL,             RuleUndef,  0        }
  };

  const char* name = idOf(actionP);

  if (name == NULL)
  {
    snprintf(errorBuf, errorBufSize, "an action that is not a name");
    return false;
  }

  name = norm(name);

  for (int i = 0; actionV[i].name != NULL; i++)
  {
    if (strcmp(actionV[i].name, name) == 0)
    {
      Rule* rP = ruleAdd(psP, rulesP);

      if (rP == NULL)
        return false;

      rP->kind    = actionV[i].kind;
      rP->methods = actionV[i].methods;
      return true;
    }
  }

  snprintf(errorBuf, errorBufSize, "unknown action '%.128s'", name);
  return false;
}



// -----------------------------------------------------------------------------
//
// assigneeParse - vc:any / json:any; a party (its uid) = the credential's issuer; a PartyCollection = its refinement
//
static bool assigneeParse(PolicySet* psP, CorNode* assigneeP, RuleList* rulesP, char* errorBuf, int errorBufSize)
{
  if (typeIs(assigneeP, "odrl:PartyCollection") == true)
    return collectionParse(psP, assigneeP, rulesP, errorBuf, errorBufSize);

  const char* uid = (typeIs(assigneeP, "odrl:Party") == true) ? idOf(odrl(assigneeP, "uid")) : idOf(assigneeP);

  if (uid == NULL)
  {
    snprintf(errorBuf, errorBufSize, "an assignee without a uid");
    return false;
  }

  Rule* rP = ruleAdd(psP, rulesP);

  if (rP == NULL)
    return false;

  if ((strcmp(uid, "vc:any") == 0) || (strcmp(uid, "json:any") == 0))
    rP->kind = RuleTrue;
  else
  {
    rP->kind = RuleIssuer;
    rP->text = uid;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// targetParse - a uid = the last path segment; an AssetCollection = its refinement
//
static bool targetParse(PolicySet* psP, CorNode* targetP, RuleList* rulesP, char* errorBuf, int errorBufSize)
{
  if (typeIs(targetP, "odrl:AssetCollection") == true)
    return collectionParse(psP, targetP, rulesP, errorBuf, errorBufSize);

  const char* uid = (typeIs(targetP, "odrl:Asset") == true) ? idOf(odrl(targetP, "uid")) : idOf(targetP);

  if (uid == NULL)
  {
    snprintf(errorBuf, errorBufSize, "a target without a uid");
    return false;
  }

  Rule* rP = ruleAdd(psP, rulesP);

  if (rP == NULL)
    return false;

  rP->kind = RuleTarget;
  rP->text = uid;
  return true;
}



// -----------------------------------------------------------------------------
//
// permissionParse - a permission's rules into the policy's one rule list
//
static bool permissionParse(PolicySet* psP, CorNode* permP, CorNode* policyP, RuleList* rulesP, char* errorBuf, int errorBufSize)
{
  CorNode* actionP   = odrl(permP, "action");
  CorNode* assigneeP = odrl(permP, "assignee");
  CorNode* targetP   = odrl(permP, "target");

  if (assigneeP == NULL)  assigneeP = odrl(policyP, "assignee");
  if (targetP   == NULL)  targetP   = odrl(policyP, "target");

  if ((actionP == NULL) || (assigneeP == NULL) || (targetP == NULL))
  {
    snprintf(errorBuf, errorBufSize, "a permission without action, assignee or target");
    return false;
  }

  if (actionParse(psP, actionP, rulesP, errorBuf, errorBufSize) == false)       return false;
  if (assigneeParse(psP, assigneeP, rulesP, errorBuf, errorBufSize) == false)   return false;
  if (targetParse(psP, targetP, rulesP, errorBuf, errorBufSize) == false)       return false;

  CorNode* constraintP = odrl(permP, "constraint");

  if (constraintP != NULL)
  {
    if (refinementParse(psP, constraintP, rulesP, errorBuf, errorBufSize) == false)
      return false;
  }

  if (corTreeLookup(permP, "ovc:constraint") != NULL)   // the PAP maps it to true
  {
    Rule* rP = ruleAdd(psP, rulesP);

    if (rP == NULL)
      return false;

    rP->kind = RuleTrue;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// policyParse - one policy into the set; false: the PAP would refuse it (errorBuf)
//
static bool policyParse(PolicySet* psP, CorNode* policyP, char* errorBuf, int errorBufSize)
{
  if ((typeIs(policyP, "odrl:Policy") == false) && (typeIs(policyP, "odrl:Agreement") == false) &&
      (typeIs(policyP, "odrl:Offer")  == false) && (typeIs(policyP, "odrl:Set")       == false))
  {
    snprintf(errorBuf, errorBufSize, "not an ODRL policy (@type)");
    return false;
  }

  const char* uid   = idOf(odrl(policyP, "uid"));
  CorNode*    permP = odrl(policyP, "permission");

  if (uid == NULL)
    uid = idOf(corTreeLookup(policyP, "@id"));

  if ((uid == NULL) || (permP == NULL))
  {
    snprintf(errorBuf, errorBufSize, "a policy without uid or permission");
    return false;
  }

  RuleList  rules = { NULL, 0, 0 };
  bool      single;

  for (CorNode* pP = listOf(permP, &single); pP != NULL; pP = (single == true) ? NULL : pP->next)
  {
    if (permissionParse(psP, pP, policyP, &rules, errorBuf, errorBufSize) == false)
      return false;
  }

  if (psP->policies == psP->policySize)
  {
    int      size = (psP->policySize == 0) ? 16 : psP->policySize * 2;
    Policy*  v    = (Policy*) corAlloc(&psP->ka, size * sizeof(Policy));

    if (v == NULL)
      return false;

    if (psP->policies > 0)
      memcpy(v, psP->policyV, psP->policies * sizeof(Policy));

    psP->policyV    = v;
    psP->policySize = size;
  }

  Policy* policyEntryP = &psP->policyV[psP->policies++];

  policyEntryP->uid = uid;
  memset(&policyEntryP->all, 0, sizeof(Rule));
  ruleSet(&policyEntryP->all, RuleAnd, &rules);
  return true;
}



// -----------------------------------------------------------------------------
//
// policyOne - a policy or an @graph of them; the number read
//
static int policyOne(PolicySet* psP, CorNode* nodeP)
{
  char     errorBuf[256];
  CorNode* graphP = (nodeP->type == CorObject) ? corTreeLookup(nodeP, "@graph") : NULL;
  int      n      = 0;

  if ((graphP != NULL) && (graphP->type == CorArray))
  {
    for (CorNode* pP = graphP->value.head; pP != NULL; pP = pP->next)
      n += policyOne(psP, pP);

    return n;
  }

  if (policyParse(psP, nodeP, errorBuf, sizeof(errorBuf)) == true)
    return 1;

  const char* uid = idOf(odrl(nodeP, "uid"));
  COR_W("fdsc: policy '%s' skipped: %s", (uid != NULL) ? uid : "?", errorBuf);
  return 0;
}



// -----------------------------------------------------------------------------
//
// setFree -
//
static void setFree(PolicySet* psP)
{
  if (psP == NULL)
    return;

  for (int i = 0; i < psP->regexes; i++)
  {
    regfree(psP->regexV[i]);
    free(psP->regexV[i]);
  }

  free(psP->regexV);
  corAllocBufferReset(&psP->ka, false);
  free(psP);
}



// -----------------------------------------------------------------------------
//
// fdscPolicySetBegin -
//
void fdscPolicySetBegin(void)
{
  setFree(buildP);

  buildP = (PolicySet*) calloc(1, sizeof(PolicySet));

  if (buildP != NULL)
    corAllocBufferInit(&buildP->ka, buildP->buf, sizeof(buildP->buf), 64 * 1024, NULL, "fdsc policies");
}



// -----------------------------------------------------------------------------
//
// fdscPolicySetAdd -
//
int fdscPolicySetAdd(char* text, int* itemsP, char* errorBuf, int errorBufSize)
{
  *itemsP = 0;

  if (buildP == NULL)
  {
    snprintf(errorBuf, errorBufSize, "out of memory");
    return -1;
  }

  //
  // The text is parsed in the set's own arena: the policies point into it for as long as the set is in use
  //
  char* copy = corAllocStrdup(&buildP->ka, text);

  if (copy == NULL)
  {
    snprintf(errorBuf, errorBufSize, "out of memory");
    return -1;
  }

  CorJson cj;
  corJsonCreate(&cj, &buildP->ka);

  CorNode* rootP = corJsonParse(&cj, copy);

  if ((rootP == NULL) || ((rootP->type != CorArray) && (rootP->type != CorObject)))
  {
    snprintf(errorBuf, errorBufSize, "not JSON - neither an array of policies nor a policy");
    return -1;
  }

  if (rootP->type == CorObject)
  {
    *itemsP = 1;
    return policyOne(buildP, rootP);
  }

  int n = 0;

  for (CorNode* itemP = rootP->value.head; itemP != NULL; itemP = itemP->next)
  {
    *itemsP += 1;

    //
    // The PAP's listing: { "id", "odrl:uid", "odrl": "<the policy as a string>", "rego" }
    //
    CorNode* odrlP = (itemP->type == CorObject) ? corTreeLookup(itemP, "odrl") : NULL;

    if ((odrlP != NULL) && (odrlP->type == CorString))
    {
      CorJson pj;
      corJsonCreate(&pj, &buildP->ka);

      char*    policyText = corAllocStrdup(&buildP->ka, odrlP->value.s);
      CorNode* policyP    = (policyText != NULL) ? corJsonParse(&pj, policyText) : NULL;

      if ((policyP != NULL) && (policyP->type == CorObject))
        n += policyOne(buildP, policyP);
      else
        COR_W("fdsc: a policy of the PAP's listing is not JSON - skipped");
    }
    else if (itemP->type == CorObject)
      n += policyOne(buildP, itemP);
  }

  return n;
}



// -----------------------------------------------------------------------------
//
// fdscPolicySetCommit -
//
void fdscPolicySetCommit(void)
{
  if (buildP == NULL)
    return;

  pthread_rwlock_wrlock(&setLock);
  PolicySet* oldP = setP;
  setP   = buildP;
  loaded = true;
  pthread_rwlock_unlock(&setLock);

  buildP = NULL;
  setFree(oldP);
}



// =============================================================================
//
// Deciding
//
// =============================================================================



// -----------------------------------------------------------------------------
//
// Ctx - one request's decision: what the words read, each read once
//
typedef struct Ctx
{
  const AuthRequest*  reqP;
  FdscIdentity*       idP;
  CorAlloc*           kaP;
  int                 method;
  long long           nowMs;
  bool                conflict;                      // an OPA eval_conflict_error: two values for one word

  bool                bodyRead;
  CorNode*            bodyP;
  bool                queryRead;
  CorNode*            queryP;

  //
  // --fdscNgsild: the entity type is the entity's - its expanded types (entityP, in entity()); before it is
  // known (authorize, when the broker will ask entity()), a rule on it is Pending
  //
  const AuthEntity*   entityP;
  bool                defer;
} Ctx;

static bool ngsild = false;                          // --fdscNgsild (fdscPolicyNgsildSet)



// -----------------------------------------------------------------------------
//
// credential - the token's Verifiable Credential: verifiableCredential, else vc. Never
// verifiablePresentation - the PAP's apisix.rego does not read it, so a token with two or more
// credentials satisfies no vc: word and no issuer assignee.
//
static CorNode* credential(Ctx* cP)
{
  if ((cP->idP == NULL) || (cP->idP->claimsP == NULL))
    return NULL;

  CorNode* vcP = corTreeLookup(cP->idP->claimsP, "verifiableCredential");

  return (vcP != NULL) ? vcP : corTreeLookup(cP->idP->claimsP, "vc");
}



// -----------------------------------------------------------------------------
//
// body - the request body as JSON, once; NULL: none, or not JSON (APISIX's opa plugin leaves it out then)
//
static CorNode* body(Ctx* cP)
{
  if (cP->bodyRead == false)
  {
    int         len  = 0;
    const char* text = cP->reqP->body(&len);

    cP->bodyRead = true;

    if ((text != NULL) && (len > 0))
    {
      char* copy = (char*) corAlloc(cP->kaP, len + 1);

      if (copy != NULL)
      {
        memcpy(copy, text, len);
        copy[len] = 0;

        CorJson cj;
        corJsonCreate(&cj, cP->kaP);
        cP->bodyP = corJsonParse(&cj, copy);
      }
    }
  }

  return cP->bodyP;
}



// -----------------------------------------------------------------------------
//
// query - the query as APISIX's opa plugin sends it: name -> value; a name given twice -> an array of its
// values; a name without a value -> true
//
static CorNode* query(Ctx* cP)
{
  if (cP->queryRead == true)
    return cP->queryP;

  cP->queryRead = true;

  if (cP->reqP->params == 0)
    return NULL;

  cP->queryP = corTreeObject(cP->kaP, NULL);

  for (int i = 0; i < cP->reqP->params; i++)
  {
    const char* key    = cP->reqP->paramKeyV[i];
    const char* value  = cP->reqP->paramValueV[i];
    CorNode*    valueP = (value[0] != 0) ? corTreeString(cP->kaP, key, value) : corTreeBoolean(cP->kaP, key, true);
    CorNode*    oldP   = corTreeLookup(cP->queryP, key);

    if (oldP == NULL)
      corTreeChildAdd(cP->queryP, valueP);
    else if (oldP->type == CorArray)
    {
      valueP->name = NULL;
      corTreeChildAdd(oldP, valueP);
    }
    else
    {
      //
      // The second value: the member becomes an array of both
      //
      CorNode* firstP = (CorNode*) corAlloc(cP->kaP, sizeof(CorNode));

      if (firstP == NULL)
        continue;

      *firstP      = *oldP;
      firstP->name = NULL;
      firstP->next = NULL;

      oldP->type       = CorArray;
      oldP->value.head = NULL;
      oldP->value.tail = NULL;

      valueP->name = NULL;
      corTreeChildAdd(oldP, firstP);
      corTreeChildAdd(oldP, valueP);
    }
  }

  return cP->queryP;
}



// -----------------------------------------------------------------------------
//
// pathOf - http:path's path: the x-stripped-path header (--fdscStrippedPath), else the request's
//
static bool strippedPathOk = false;                 // --fdscStrippedPath

static const char* pathOf(Ctx* cP)
{
  //
  // A client can send the header: it is read only when a gateway in front sets it (and drops the client's)
  //
  const char* stripped = (strippedPathOk == true) ? cP->reqP->header("x-stripped-path") : NULL;

  return (stripped != NULL) ? stripped : cP->reqP->path;
}



// -----------------------------------------------------------------------------
//
// fdscPolicyStrippedPathSet -
//
void fdscPolicyStrippedPathSet(bool on)
{
  strippedPathOk = on;
}



// -----------------------------------------------------------------------------
//
// lastSegment - the last '/'-separated part of the path (apisix.rego's target)
//
static const char* lastSegment(const char* path)
{
  const char* slash = strrchr(path, '/');

  return (slash != NULL) ? &slash[1] : path;
}



// -----------------------------------------------------------------------------
//
// entityType - ngsi-ld:entityType as the PAP's Rego finds it: the third ':' part of the last path segment
// (urn:ngsi-ld:<Type>:...), else the query's 'type', else the body's 'type' - as sent, never expanded
//
static CorNode* entityType(Ctx* cP)
{
  const char* last = lastSegment(cP->reqP->path);
  const char* c1   = strchr(last, ':');
  const char* c2   = (c1 != NULL) ? strchr(c1 + 1, ':') : NULL;

  if (c2 != NULL)
  {
    const char* c3  = strchr(c2 + 1, ':');
    int         len = (c3 != NULL) ? (int) (c3 - c2 - 1) : (int) strlen(c2 + 1);
    char*       s   = (char*) corAlloc(cP->kaP, len + 1);

    if (s == NULL)
      return NULL;

    memcpy(s, c2 + 1, len);
    s[len] = 0;
    return corTreeString(cP->kaP, NULL, s);
  }

  CorNode* qP = query(cP);
  CorNode* tP = (qP != NULL) ? corTreeLookup(qP, "type") : NULL;

  if (tP != NULL)
    return tP;

  CorNode* bP = body(cP);

  return ((bP != NULL) && (bP->type == CorObject)) ? corTreeLookup(bP, "type") : NULL;
}



// -----------------------------------------------------------------------------
//
// dottedPath - a value at "a.b.c" ("$." in front dropped) under nodeP
//
static CorNode* dottedPath(Ctx* cP, CorNode* nodeP, const char* path)
{
  if ((nodeP == NULL) || (path == NULL))
    return NULL;

  if (strncmp(path, "$.", 2) == 0)
    path += 2;

  char* copy = corAllocStrdup(cP->kaP, path);
  char* save = NULL;

  if (copy == NULL)
    return NULL;

  for (char* key = strtok_r(copy, ".", &save); (key != NULL) && (nodeP != NULL); key = strtok_r(NULL, ".", &save))
    nodeP = (nodeP->type == CorObject) ? corTreeLookup(nodeP, key) : NULL;

  return nodeP;
}



static bool deepEqual(CorNode* aP, CorNode* bP);



// -----------------------------------------------------------------------------
//
// roleNames - vc:role / dome-op:role: the names of the credential subject's role entry for the
// organisation. Two entries with different names: two values for one word - OPA's eval_conflict_error.
//
static CorNode* roleNames(Ctx* cP, const char* listName, const char* namesName)
{
  CorNode* vcP      = credential(cP);
  CorNode* subjectP = ((vcP != NULL) && (vcP->type == CorObject)) ? corTreeLookup(vcP, "credentialSubject") : NULL;
  CorNode* rolesP   = ((subjectP != NULL) && (subjectP->type == CorObject)) ? corTreeLookup(subjectP, listName) : NULL;
  CorNode* foundP   = NULL;

  if ((rolesP == NULL) || (rolesP->type != CorArray))
    return NULL;

  for (CorNode* roleP = rolesP->value.head; roleP != NULL; roleP = roleP->next)
  {
    CorNode* targetP = (roleP->type == CorObject) ? corTreeLookup(roleP, "target") : NULL;
    CorNode* namesP  = (roleP->type == CorObject) ? corTreeLookup(roleP, namesName) : NULL;

    if ((targetP == NULL) || (targetP->type != CorString) || (strcmp(targetP->value.s, organization) != 0) || (namesP == NULL))
      continue;

    if ((foundP != NULL) && (deepEqual(foundP, namesP) == false))
    {
      cP->conflict = true;
      return NULL;
    }

    foundP = namesP;
  }

  return foundP;
}



// -----------------------------------------------------------------------------
//
// leftValue - what a constraint's leftOperand reads; NULL: undefined
//
static CorNode* leftValue(Ctx* cP, Rule* rP)
{
  CorNode* vcP = NULL;

  switch (rP->left)
  {
  case LeftEntityType:
    return entityType(cP);

  case LeftVcRole:
    return roleNames(cP, "roles", "names");

  case LeftDomeRole:
    return roleNames(cP, "rolesAndDuties", "roleNames");

  case LeftVcType:
    vcP = credential(cP);
    vcP = ((vcP != NULL) && (vcP->type == CorObject)) ? corTreeLookup(vcP, "type") : NULL;

    if ((vcP != NULL) && (vcP->type == CorString))   // a string type: wrapped in an array
    {
      CorNode* arrayP = corTreeArray(cP->kaP, NULL);
      CorNode* oneP   = corTreeString(cP->kaP, NULL, vcP->value.s);

      corTreeChildAdd(arrayP, oneP);
      return arrayP;
    }
    return vcP;

  case LeftVcIssuer:
    vcP = credential(cP);
    return ((vcP != NULL) && (vcP->type == CorObject)) ? corTreeLookup(vcP, "issuer") : NULL;

  case LeftHttpPath:
    return corTreeString(cP->kaP, NULL, pathOf(cP));

  case LeftBodyValue:
    return dottedPath(cP, body(cP), rP->leftArg);

  case LeftDateTime:
    return corTreeInteger(cP->kaP, NULL, cP->nowMs);

  case LeftDayOfWeek:
  case LeftHourOfDay:
    {
      time_t    secs = (time_t) (cP->nowMs / 1000);
      struct tm tm;

      gmtime_r(&secs, &tm);

      if (rP->left == LeftHourOfDay)
        return corTreeInteger(cP->kaP, NULL, tm.tm_hour);

      return corTreeInteger(cP->kaP, NULL, (tm.tm_wday + 6) % 7);   // 0 = Monday
    }

  case LeftVcClaim:
    return dottedPath(cP, credential(cP), rP->leftArg);

  case LeftCredentialSubjectType:
    vcP = credential(cP);
    return dottedPath(cP, vcP, "credentialSubject.type");

  case LeftTmfLifecycle:
    return dottedPath(cP, body(cP), "lifeCycleStatus");

  case LeftTmfResource:
    {
      //
      // The last path segment that does not contain "ngsi-ld"
      //
      char*       copy  = corAllocStrdup(cP->kaP, cP->reqP->path);
      char*       save  = NULL;
      const char* found = NULL;

      if (copy == NULL)
        return NULL;

      for (char* seg = strtok_r(copy, "/", &save); seg != NULL; seg = strtok_r(NULL, "/", &save))
      {
        if (strstr(seg, "ngsi-ld") == NULL)
          found = seg;
      }

      return (found != NULL) ? corTreeString(cP->kaP, NULL, found) : NULL;
    }

  case LeftLiteral:
    return corTreeString(cP->kaP, NULL, rP->leftArg);

  case LeftUndef:
    return NULL;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// rank - Rego's order of types: null < boolean < number < string < array < object
//
static int rank(CorNode* nodeP)
{
  switch (nodeP->type)
  {
  case CorNull:     return 0;
  case CorBoolean:  return 1;
  case CorInt:
  case CorFloat:    return 2;
  case CorString:   return 3;
  case CorArray:    return 4;
  case CorObject:   return 5;
  default:          return 6;
  }
}

static double number(CorNode* nodeP)
{
  return (nodeP->type == CorInt) ? (double) nodeP->value.i : nodeP->value.f;
}



// -----------------------------------------------------------------------------
//
// compare - Rego's total order: <0, 0, >0
//
static int compare(CorNode* aP, CorNode* bP)
{
  int ra = rank(aP);
  int rb = rank(bP);

  if (ra != rb)
    return ra - rb;

  switch (ra)
  {
  case 0:
    return 0;

  case 1:
    return (int) aP->value.b - (int) bP->value.b;

  case 2:
    {
      double a = number(aP);
      double b = number(bP);
      return (a < b) ? -1 : (a > b) ? 1 : 0;
    }

  case 3:
    return strcmp(aP->value.s, bP->value.s);

  case 4:
    {
      CorNode* xP = aP->value.head;
      CorNode* yP = bP->value.head;

      for (; (xP != NULL) && (yP != NULL); xP = xP->next, yP = yP->next)
      {
        int c = compare(xP, yP);

        if (c != 0)
          return c;
      }

      return (xP != NULL) ? 1 : (yP != NULL) ? -1 : 0;
    }

  default:
    return deepEqual(aP, bP) ? 0 : 1;
  }
}



// -----------------------------------------------------------------------------
//
// deepEqual - Rego's ==: same type (a number is a number, int or float), same value, members regardless
// of order, elements in order
//
static bool deepEqual(CorNode* aP, CorNode* bP)
{
  if (rank(aP) != rank(bP))
    return false;

  if (aP->type == CorObject)
  {
    int na = 0;
    int nb = 0;

    for (CorNode* xP = aP->value.head; xP != NULL; xP = xP->next)
    {
      CorNode* yP = corTreeLookup(bP, xP->name);

      if ((yP == NULL) || (deepEqual(xP, yP) == false))
        return false;
      ++na;
    }

    for (CorNode* yP = bP->value.head; yP != NULL; yP = yP->next)
      ++nb;

    return na == nb;
  }

  return compare(aP, bP) == 0;
}



// -----------------------------------------------------------------------------
//
// in - Rego's 'x in collection': an element of an array, a member value of an object; false for a scalar
//
static bool in(CorNode* xP, CorNode* collectionP)
{
  if ((collectionP->type != CorArray) && (collectionP->type != CorObject))
    return false;

  for (CorNode* eP = collectionP->value.head; eP != NULL; eP = eP->next)
  {
    if (deepEqual(xP, eP) == true)
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// inPath - http:isInPath: both split on '/' (outer '/' trimmed); r is a prefix of l, '*' one any segment
//
static bool inPath(const char* l, const char* r)
{
  while (*l == '/') ++l;
  while (*r == '/') ++r;

  int lLen = strlen(l);
  int rLen = strlen(r);

  while ((lLen > 0) && (l[lLen - 1] == '/')) --lLen;
  while ((rLen > 0) && (r[rLen - 1] == '/')) --rLen;

  const char* lEnd = &l[lLen];
  const char* rEnd = &r[rLen];

  //
  // Segment by segment - an empty path is one empty segment, as Rego's split("", "/") is [""]
  //
  while (true)
  {
    const char* ls = memchr(l, '/', lEnd - l);
    const char* rs = memchr(r, '/', rEnd - r);
    int         ln = (ls != NULL) ? (int) (ls - l) : (int) (lEnd - l);
    int         rn = (rs != NULL) ? (int) (rs - r) : (int) (rEnd - r);

    bool star = (rn == 1) && (r[0] == '*');

    if ((star == false) && ((ln != rn) || (strncmp(l, r, ln) != 0)))
      return false;

    if (rs == NULL)                                  // every segment of r matched
      return true;

    if (ls == NULL)                                  // r has more segments than l
      return false;

    l = ls + 1;
    r = rs + 1;
  }
}



// -----------------------------------------------------------------------------
//
// constraintEval -
//
static Tri opEval(Ctx* cP, Rule* rP, CorNode* lP, CorNode* vP);
static Tri entityTypeEval(Ctx* cP, Rule* rP);

static Tri constraintEval(Ctx* cP, Rule* rP)
{
  if ((rP->left == LeftEntityType) && (ngsild == true))
  {
    if (cP->entityP != NULL)
      return entityTypeEval(cP, rP);

    if (cP->defer == true)
      return TriPending;
  }

  CorNode* lP = leftValue(cP, rP);
  CorNode* vP;

  if (rP->right == RightNow)
    vP = corTreeInteger(cP->kaP, NULL, cP->nowMs);
  else if (rP->right == RightIssuer)
  {
    CorNode* vcP = credential(cP);
    vP = ((vcP != NULL) && (vcP->type == CorObject)) ? corTreeLookup(vcP, "issuer") : NULL;
  }
  else
    vP = rP->rightP;

  return opEval(cP, rP, lP, vP);
}



// -----------------------------------------------------------------------------
//
// rightExpanded - --fdscNgsild: an entity type of a policy as an IRI - a short name expanded with the
// broker's default context (what the policy's author meant by it, not the request's @context)
//
static CorNode* rightExpanded(Ctx* cP, CorNode* vP)
{
  if (vP->type == CorString)
    return corTreeString(cP->kaP, NULL, cP->reqP->expandDefault(vP->value.s));

  if (vP->type != CorArray)
    return vP;

  CorNode* arrayP = corTreeArray(cP->kaP, NULL);

  for (CorNode* eP = vP->value.head; eP != NULL; eP = eP->next)
    corTreeChildAdd(arrayP, (eP->type == CorString) ? corTreeString(cP->kaP, NULL, cP->reqP->expandDefault(eP->value.s)) : eP);

  return arrayP;
}



// -----------------------------------------------------------------------------
//
// entityTypeEval - --fdscNgsild, in entity(): ngsi-ld:entityType against the entity's expanded types - a
// positive operator holds when it holds for ONE of them, a negative one (neq, isNoneOf) when it holds for
// every one
//
static Tri entityTypeEval(Ctx* cP, Rule* rP)
{
  if ((rP->right != RightValue) || (rP->rightP == NULL) || (cP->entityP->types == 0))
    return TriUndef;

  CorNode* vP       = rightExpanded(cP, rP->rightP);
  bool     negative = (rP->op == OpNeq) || (rP->op == OpIsNoneOf);
  bool     any      = false;
  bool     all      = true;

  for (int i = 0; i < cP->entityP->types; i++)
  {
    Tri t = opEval(cP, rP, corTreeString(cP->kaP, NULL, cP->entityP->typeV[i]), vP);

    if (t == TriUndef)
      return TriUndef;

    any = any || (t == TriTrue);
    all = all && (t == TriTrue);
  }

  return ((negative == true) ? all : any) ? TriTrue : TriFalse;
}



// -----------------------------------------------------------------------------
//
// opEval - an operator on two defined values
//
static Tri opEval(Ctx* cP, Rule* rP, CorNode* lP, CorNode* vP)
{
  (void) cP;

  if ((lP == NULL) || (vP == NULL))
    return TriUndef;

  bool b = false;

  switch (rP->op)
  {
  case OpEq:        b = deepEqual(lP, vP);            break;
  case OpNeq:       b = !deepEqual(lP, vP);           break;
  case OpGt:        b = compare(lP, vP) > 0;          break;
  case OpGteq:      b = compare(lP, vP) >= 0;         break;
  case OpLt:        b = compare(lP, vP) < 0;          break;
  case OpLteq:      b = compare(lP, vP) <= 0;         break;
  case OpHasPart:   b = in(vP, lP);                   break;
  case OpIsPartOf:  b = in(vP, lP);                   break;   // as the PAP's operator.rego has it: the same as hasPart
  case OpIsAnyOf:   b = in(lP, vP);                   break;
  case OpIsNoneOf:  b = !in(lP, vP);                  break;
  case OpIsAllOf:   b = deepEqual(lP, vP);            break;

  case OpRegex:
    if ((lP->type != CorString) || (rP->regexP == NULL))   // a builtin's type error: undefined
      return TriUndef;
    b = (regexec(rP->regexP, lP->value.s, 0, NULL, 0) == 0);
    break;

  case OpIsInPath:
    if ((lP->type != CorString) || (vP->type != CorString))
      return TriUndef;
    b = inPath(lP->value.s, vP->value.s);
    break;
  }

  return (b == true) ? TriTrue : TriFalse;
}



// -----------------------------------------------------------------------------
//
// ruleEval -
//
static Tri ruleEval(Ctx* cP, Rule* rP)
{
  switch (rP->kind)
  {
  case RuleTrue:
    return TriTrue;

  case RuleUndef:
    return TriUndef;

  case RuleAction:
    return ((cP->method & rP->methods) != 0) ? TriTrue : TriFalse;

  case RuleIssuer:
    {
      CorNode* vcP  = credential(cP);
      CorNode* issP = ((vcP != NULL) && (vcP->type == CorObject)) ? corTreeLookup(vcP, "issuer") : NULL;

      if (issP == NULL)
        return TriUndef;

      return ((issP->type == CorString) && (strcmp(issP->value.s, rP->text) == 0)) ? TriTrue : TriFalse;
    }

  case RuleTarget:
    return (strcmp(lastSegment(cP->reqP->path), rP->text) == 0) ? TriTrue : TriFalse;

  case RuleConstraint:
    return constraintEval(cP, rP);

  case RuleAnd:
  case RuleOr:
  case RuleXone:
    {
      //
      // The elements are an array literal in the Rego: one undefined element makes the whole undefined
      //
      int trues   = 0;
      int undef   = 0;
      int pending = 0;

      for (int i = 0; i < rP->children; i++)
      {
        Tri t = ruleEval(cP, &rP->childV[i]);

        if (t == TriTrue)
          ++trues;
        else if (t == TriUndef)
          ++undef;
        else if (t == TriPending)
          ++pending;
      }

      if (undef > 0)
        return TriUndef;

      int falses = rP->children - trues - pending;

      if (rP->kind == RuleAnd)
        return (falses > 0) ? TriFalse : (pending > 0) ? TriPending : TriTrue;

      if (rP->kind == RuleOr)
        return (trues > 0) ? TriTrue : (pending > 0) ? TriPending : TriFalse;

      if ((trues > 1) || ((trues == 0) && (pending == 0)))
        return TriFalse;

      return (pending > 0) ? TriPending : TriTrue;
    }
  }

  return TriUndef;
}



// -----------------------------------------------------------------------------
//
// policyAllows - every rule of the policy true (its body: each line must hold, the first that does not ends
// it); Pending: no rule fails, and one depends on the entity
//
static Tri policyAllows(Ctx* cP, Policy* pP)
{
  bool pending = false;

  for (int i = 0; i < pP->all.children; i++)
  {
    Tri t = ruleEval(cP, &pP->all.childV[i]);

    if (t == TriPending)
      pending = true;
    else if (t != TriTrue)
      return TriFalse;
  }

  return (pending == true) ? TriPending : TriTrue;
}



// -----------------------------------------------------------------------------
//
// actionOf - --fdscNgsild: the action of an NGSI-LD operation, whatever its HTTP method - a query in a POST
// body is a read, a PUT is a modification. 0: not one of these (the method decides).
//
static int actionOf(uint64_t op)
{
  const uint64_t reads   = LdOpRetrieveEntity | LdOpQueryEntities | LdOpBatchQuery |
                           LdOpRetrieveEntityTypes | LdOpRetrieveEntityTypeDetails | LdOpRetrieveEntityTypeInfo |
                           LdOpRetrieveAttrTypes   | LdOpRetrieveAttrTypeDetails   | LdOpRetrieveAttrTypeInfo |
                           LdOpRetrieveTemporal    | LdOpQueryTemporal;
  const uint64_t creates = LdOpCreateEntity | LdOpBatchCreate;
  const uint64_t updates = LdOpUpdateEntity | LdOpAppendAttrs | LdOpUpdateAttrs | LdOpMergeEntity | LdOpReplaceEntity |
                           LdOpReplaceAttr  | LdOpBatchUpdate | LdOpBatchMerge  |
                           LdOpAppendAttrsTemporal | LdOpUpdateAttrInstanceTemporal;
  const uint64_t deletes = LdOpDeleteEntity | LdOpDeleteAttr | LdOpPurgeEntity | LdOpBatchDelete |
                           LdOpDeleteAttrsTemporal | LdOpDeleteAttrInstanceTemporal | LdOpDeleteTemporal;

  if ((op & reads)   != 0)  return M_GET;
  if ((op & creates) != 0)  return M_POST;
  if ((op & updates) != 0)  return M_PATCH;
  if ((op & deletes) != 0)  return M_DELETE;
  if ((op & (LdOpBatchUpsert | LdOpUpsertTemporal)) != 0)  return M_POST | M_PATCH;   // a create, or an update: either action allows it

  return 0;
}



// -----------------------------------------------------------------------------
//
// methodOf -
//
static int methodOf(const char* method)
{
  if (strcmp(method, "GET")    == 0)  return M_GET;
  if (strcmp(method, "POST")   == 0)  return M_POST;
  if (strcmp(method, "PUT")    == 0)  return M_PUT;
  if (strcmp(method, "PATCH")  == 0)  return M_PATCH;
  if (strcmp(method, "DELETE") == 0)  return M_DELETE;

  return 0;                                          // HEAD, OPTIONS: no action names them
}



// -----------------------------------------------------------------------------
//
// decide - every policy, as OPA evaluates every 'allow if' of policy.main - an error in any one of them
// (eval_conflict_error) is the decision's error, whatever the others say
//
static AuthVerdict decide(const AuthRequest* reqP, FdscIdentity* idP, const AuthEntity* entityP, AuthAnswer* answerP)
{
  CorAlloc         ka;
  char             buf[4096];
  Ctx              ctx;
  struct timespec  now;

  clock_gettime(CLOCK_REALTIME, &now);
  corAllocBufferInit(&ka, buf, sizeof(buf), 8192, NULL, "fdsc decision");

  memset(&ctx, 0, sizeof(ctx));
  ctx.reqP    = reqP;
  ctx.idP     = idP;
  ctx.kaP     = &ka;
  ctx.method  = ((ngsild == true) && (actionOf(reqP->op) != 0)) ? actionOf(reqP->op) : methodOf(reqP->method);

  //
  // An upsert, entity by entity: an update of one that exists, a create of one that does not
  //
  if ((ngsild == true) && (entityP != NULL) && ((reqP->op & (LdOpBatchUpsert | LdOpUpsertTemporal)) != 0))
    ctx.method = (entityP->stored == true) ? M_PATCH : M_POST;
  ctx.nowMs   = (long long) now.tv_sec * 1000 + now.tv_nsec / 1000000;
  ctx.entityP = entityP;
  ctx.defer   = (ngsild == true) && (entityP == NULL) && (reqP->entities == true) && (idP != NULL);

  bool allowed = false;
  bool pending = false;

  pthread_rwlock_rdlock(&setLock);

  for (int i = 0; (setP != NULL) && (i < setP->policies); i++)
  {
    Tri t = policyAllows(&ctx, &setP->policyV[i]);

    if (t == TriTrue)
      allowed = true;
    else if (t == TriPending)
      pending = true;
  }

  pthread_rwlock_unlock(&setLock);
  corAllocBufferReset(&ka, false);

  if (ctx.conflict == true)
  {
    answerP->detail = "the credential has more than one role list for this organization";
    return AuthUnavailable;
  }

  //
  // authorize: a policy that holds whatever the entity - entity() has nothing left to ask; one that holds
  // depending on the entity - entity() decides
  //
  if ((entityP == NULL) && (idP != NULL))
    idP->entityFree = allowed;

  if ((allowed == true) || (pending == true))
    return AuthAllow;

  answerP->detail = (entityP != NULL) ? "no policy allows this entity" : "no policy allows the request";
  return AuthDeny;
}



// -----------------------------------------------------------------------------
//
// fdscPolicyDecide -
//
AuthVerdict fdscPolicyDecide(const AuthRequest* reqP, FdscIdentity* idP, AuthAnswer* answerP)
{
  return decide(reqP, idP, NULL, answerP);
}



// -----------------------------------------------------------------------------
//
// fdscPolicyEntity -
//
AuthVerdict fdscPolicyEntity(const AuthRequest* reqP, FdscIdentity* idP, const AuthEntity* entityP, AuthAnswer* answerP)
{
  if ((ngsild == false) || ((idP != NULL) && (idP->entityFree == true)))
    return AuthAllow;

  return decide(reqP, idP, entityP, answerP);
}



// -----------------------------------------------------------------------------
//
// fdscPolicyNgsildSet -
//
void fdscPolicyNgsildSet(bool on)
{
  ngsild = on;
}



// -----------------------------------------------------------------------------
//
// fdscPolicyOrganizationSet -
//
void fdscPolicyOrganizationSet(const char* did)
{
  if (did != NULL)
    snprintf(organization, sizeof(organization), "%s", did);
}



// -----------------------------------------------------------------------------
//
// fdscPolicyLoaded -
//
bool fdscPolicyLoaded(void)
{
  pthread_rwlock_rdlock(&setLock);
  bool b = loaded;
  pthread_rwlock_unlock(&setLock);

  return b;
}



// -----------------------------------------------------------------------------
//
// fdscPolicyCount -
//
int fdscPolicyCount(void)
{
  pthread_rwlock_rdlock(&setLock);
  int n = (setP != NULL) ? setP->policies : 0;
  pthread_rwlock_unlock(&setLock);

  return n;
}



// -----------------------------------------------------------------------------
//
// fdscPolicyRelease -
//
void fdscPolicyRelease(void)
{
  pthread_rwlock_wrlock(&setLock);
  PolicySet* oldP = setP;
  setP   = NULL;
  loaded = false;
  pthread_rwlock_unlock(&setLock);

  setFree(oldP);
  setFree(buildP);
  buildP = NULL;
}
