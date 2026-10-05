//
// FILE            seJsonSchema.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strcmp, strlen
#include <math.h>                                     // floor

#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "serviceExecution/seJsonSchema.h"            // Own interface



// -----------------------------------------------------------------------------
//
// number - a numeric node's value
//
static bool number(CorNode* nodeP, double* dP)
{
  if (nodeP == NULL)
    return false;

  if (nodeP->type == CorInt)   { *dP = (double) nodeP->value.i; return true; }
  if (nodeP->type == CorFloat) { *dP = nodeP->value.f;          return true; }

  return false;
}



// -----------------------------------------------------------------------------
//
// typeIs - does the value have the JSON Schema type 'type'?
//
static bool typeIs(CorNode* valueP, const char* type)
{
  switch (valueP->type)
  {
  case CorString:  return strcmp(type, "string")  == 0;
  case CorBoolean: return strcmp(type, "boolean") == 0;
  case CorNull:    return strcmp(type, "null")    == 0;
  case CorObject:  return strcmp(type, "object")  == 0;
  case CorArray:   return strcmp(type, "array")   == 0;
  case CorInt:     return (strcmp(type, "integer") == 0) || (strcmp(type, "number") == 0);
  case CorFloat:
    if (strcmp(type, "number") == 0)
      return true;
    return (strcmp(type, "integer") == 0) && (valueP->value.f == floor(valueP->value.f));
  default:
    return false;
  }
}



// -----------------------------------------------------------------------------
//
// sameValue - JSON equality, for enum
//
static bool sameValue(CorNode* aP, CorNode* bP)
{
  double a, b;

  if (number(aP, &a) && number(bP, &b))
    return a == b;

  if (aP->type != bP->type)
    return false;

  switch (aP->type)
  {
  case CorString:  return strcmp(aP->value.s, bP->value.s) == 0;
  case CorBoolean: return aP->value.b == bP->value.b;
  case CorNull:    return true;
  case CorObject:
  case CorArray:
  {
    CorNode* xP = aP->value.head;
    CorNode* yP = bP->value.head;

    for (; (xP != NULL) && (yP != NULL); xP = xP->next, yP = yP->next)
    {
      if ((aP->type == CorObject) && (strcmp(xP->name, yP->name) != 0))
        return false;
      if (sameValue(xP, yP) == false)
        return false;
    }
    return (xP == NULL) && (yP == NULL);
  }
  default:
    return false;
  }
}



// -----------------------------------------------------------------------------
//
// fail -
//
static bool fail(char* errBuf, int errBufSize, const char* path, const char* what)
{
  snprintf(errBuf, errBufSize, "%s: %s", (path[0] != 0) ? path : "the input", what);
  return false;
}



// -----------------------------------------------------------------------------
//
// seJsonSchemaCheck -
//
bool seJsonSchemaCheck(CorNode* schemaP, CorNode* valueP, const char* path, char* errBuf, int errBufSize)
{
  char what[256];

  if (schemaP == NULL)
    return true;

  if (schemaP->type == CorBoolean)
    return (schemaP->value.b == true) ? true : fail(errBuf, errBufSize, path, "not allowed");

  if (schemaP->type != CorObject)
    return true;

  //
  // type
  //
  CorNode* typeP = corTreeLookup(schemaP, "type");

  if ((typeP != NULL) && (typeP->type == CorString) && (typeIs(valueP, typeP->value.s) == false))
  {
    snprintf(what, sizeof(what), "must be of type %s", typeP->value.s);
    return fail(errBuf, errBufSize, path, what);
  }
  else if ((typeP != NULL) && (typeP->type == CorArray))
  {
    bool any = false;

    for (CorNode* tP = typeP->value.head; (tP != NULL) && (any == false); tP = tP->next)
      any = (tP->type == CorString) && typeIs(valueP, tP->value.s);

    if (any == false)
      return fail(errBuf, errBufSize, path, "is of none of the types its schema allows");
  }

  //
  // enum
  //
  CorNode* enumP = corTreeLookup(schemaP, "enum");

  if ((enumP != NULL) && (enumP->type == CorArray))
  {
    bool any = false;

    for (CorNode* eP = enumP->value.head; (eP != NULL) && (any == false); eP = eP->next)
      any = sameValue(eP, valueP);

    if (any == false)
      return fail(errBuf, errBufSize, path, "is not one of the values its schema enumerates");
  }

  //
  // numbers
  //
  double v, limit;

  if (number(valueP, &v))
  {
    if (number(corTreeLookup(schemaP, "minimum"), &limit) && (v < limit))
    {
      snprintf(what, sizeof(what), "must be at least %g", limit);
      return fail(errBuf, errBufSize, path, what);
    }
    if (number(corTreeLookup(schemaP, "maximum"), &limit) && (v > limit))
    {
      snprintf(what, sizeof(what), "must be at most %g", limit);
      return fail(errBuf, errBufSize, path, what);
    }
    if (number(corTreeLookup(schemaP, "exclusiveMinimum"), &limit) && (v <= limit))
    {
      snprintf(what, sizeof(what), "must be greater than %g", limit);
      return fail(errBuf, errBufSize, path, what);
    }
    if (number(corTreeLookup(schemaP, "exclusiveMaximum"), &limit) && (v >= limit))
    {
      snprintf(what, sizeof(what), "must be less than %g", limit);
      return fail(errBuf, errBufSize, path, what);
    }
  }

  //
  // strings
  //
  if (valueP->type == CorString)
  {
    double len = (double) strlen(valueP->value.s);

    if (number(corTreeLookup(schemaP, "minLength"), &limit) && (len < limit))
      return fail(errBuf, errBufSize, path, "is too short");
    if (number(corTreeLookup(schemaP, "maxLength"), &limit) && (len > limit))
      return fail(errBuf, errBufSize, path, "is too long");
  }

  //
  // arrays
  //
  if (valueP->type == CorArray)
  {
    CorNode* itemsP = corTreeLookup(schemaP, "items");
    int      n      = 0;
    char     subPath[256];

    for (CorNode* iP = valueP->value.head; iP != NULL; iP = iP->next, n++)
    {
      snprintf(subPath, sizeof(subPath), "%s[%d]", path, n);

      if ((itemsP != NULL) && (seJsonSchemaCheck(itemsP, iP, subPath, errBuf, errBufSize) == false))
        return false;
    }

    if (number(corTreeLookup(schemaP, "minItems"), &limit) && (n < limit))
      return fail(errBuf, errBufSize, path, "has too few items");
    if (number(corTreeLookup(schemaP, "maxItems"), &limit) && (n > limit))
      return fail(errBuf, errBufSize, path, "has too many items");
  }

  //
  // objects
  //
  if (valueP->type == CorObject)
  {
    CorNode* propertiesP = corTreeLookup(schemaP, "properties");
    CorNode* requiredP   = corTreeLookup(schemaP, "required");
    CorNode* additionalP = corTreeLookup(schemaP, "additionalProperties");
    char     subPath[256];

    for (CorNode* rP = (requiredP != NULL) && (requiredP->type == CorArray) ? requiredP->value.head : NULL; rP != NULL; rP = rP->next)
    {
      if ((rP->type == CorString) && (corTreeLookup(valueP, rP->value.s) == NULL))
      {
        snprintf(what, sizeof(what), "'%s' is required", rP->value.s);
        return fail(errBuf, errBufSize, path, what);
      }
    }

    for (CorNode* mP = valueP->value.head; mP != NULL; mP = mP->next)
    {
      CorNode* propSchemaP = ((propertiesP != NULL) && (propertiesP->type == CorObject)) ? corTreeLookup(propertiesP, mP->name) : NULL;

      snprintf(subPath, sizeof(subPath), "%s%s%s", path, (path[0] != 0) ? "." : "", mP->name);

      if (propSchemaP != NULL)
      {
        if (seJsonSchemaCheck(propSchemaP, mP, subPath, errBuf, errBufSize) == false)
          return false;
      }
      else if (additionalP != NULL)
      {
        if ((additionalP->type == CorBoolean) && (additionalP->value.b == false))
        {
          snprintf(what, sizeof(what), "'%s' is not a member its schema allows", mP->name);
          return fail(errBuf, errBufSize, path, what);
        }

        if (seJsonSchemaCheck(additionalP, mP, subPath, errBuf, errBufSize) == false)
          return false;
      }
    }
  }

  return true;
}
