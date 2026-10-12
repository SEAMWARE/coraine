//
// FILE            fdscJwt.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                 // bool
#include <string.h>                                  // strchr, strcmp, strlen

#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corJson/CorJson.h"                         // CorJson
#include "corJson/corJsonCreate.h"                   // corJsonCreate
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "fdscBase64.h"                              // fdscBase64UrlDecode
#include "fdscKeys.h"                                // fdscKeysVerify
#include "fdscJwt.h"                                 // Own interface



// -----------------------------------------------------------------------------
//
// partJson - a base64url part decoded and parsed as a JSON object, in kaP; NULL: not one
//
static CorNode* partJson(const char* part, int len, CorAlloc* kaP)
{
  char* text = (char*) corAlloc(kaP, len * 3 / 4 + 4);

  if (text == NULL)
    return NULL;

  int n = fdscBase64UrlDecode(part, len, (unsigned char*) text);

  if (n <= 0)
    return NULL;

  text[n] = 0;

  CorJson cj;
  corJsonCreate(&cj, kaP);

  CorNode* nodeP = corJsonParse(&cj, text);

  return ((nodeP != NULL) && (nodeP->type == CorObject)) ? nodeP : NULL;
}



// -----------------------------------------------------------------------------
//
// numericDate - a NumericDate claim (RFC 7519 § 2), false: absent or not a number
//
static bool numericDate(CorNode* claimsP, const char* name, long long* valueP)
{
  CorNode* nodeP = corTreeLookup(claimsP, name);

  if (nodeP == NULL)
    return false;

  if (nodeP->type == CorInt)
    *valueP = nodeP->value.i;
  else if (nodeP->type == CorFloat)
    *valueP = (long long) nodeP->value.f;
  else
    return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// audienceHas - aud is the audience, or an array holding it (RFC 7519 § 4.1.3)
//
static bool audienceHas(CorNode* claimsP, const char* audience)
{
  CorNode* audP = corTreeLookup(claimsP, "aud");

  if (audP == NULL)
    return false;

  if (audP->type == CorString)
    return strcmp(audP->value.s, audience) == 0;

  if (audP->type == CorArray)
  {
    for (CorNode* aP = audP->value.head; aP != NULL; aP = aP->next)
    {
      if ((aP->type == CorString) && (strcmp(aP->value.s, audience) == 0))
        return true;
    }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// fdscJwtVerify -
//
FdscJwtCheck fdscJwtVerify(const char* token, const FdscJwtExpect* expectP, CorAlloc* kaP, CorNode** claimsPP)
{
  const char* dot1 = strchr(token, '.');
  const char* dot2 = (dot1 != NULL) ? strchr(dot1 + 1, '.') : NULL;

  if ((dot1 == NULL) || (dot2 == NULL) || (strchr(dot2 + 1, '.') != NULL))
    return FdscJwtMalformed;

  CorNode* headerP = partJson(token, dot1 - token, kaP);
  CorNode* claimsP = partJson(dot1 + 1, dot2 - dot1 - 1, kaP);

  if ((headerP == NULL) || (claimsP == NULL))
    return FdscJwtMalformed;

  CorNode*    algP = corTreeLookup(headerP, "alg");
  CorNode*    kidP = corTreeLookup(headerP, "kid");
  const char* alg  = ((algP != NULL) && (algP->type == CorString)) ? algP->value.s : NULL;
  const char* kid  = ((kidP != NULL) && (kidP->type == CorString)) ? kidP->value.s : NULL;

  if (alg == NULL)
    return FdscJwtMalformed;

  const char*    sigText = dot2 + 1;
  int            sigLen  = strlen(sigText);
  unsigned char* sig     = (unsigned char*) corAlloc(kaP, sigLen * 3 / 4 + 4);

  if (sig == NULL)
    return FdscJwtMalformed;

  int n = fdscBase64UrlDecode(sigText, sigLen, sig);

  if (n <= 0)
    return FdscJwtMalformed;

  FdscKeyCheck keyCheck = fdscKeysVerify(kid, alg, token, dot2 - token, sig, n);

  if (keyCheck == FdscKeyUnknown)
    return FdscJwtUnknownKey;
  if (keyCheck == FdscKeyBadAlg)
    return FdscJwtBadAlg;
  if (keyCheck != FdscKeyOk)
    return FdscJwtBadSignature;

  //
  // The claims - only once the signature holds: what an unsigned token says is nothing
  //
  long long exp = 0;
  long long nbf = 0;

  if (expectP->now != 0)
  {
    if ((numericDate(claimsP, "exp", &exp) == false) || (expectP->now > exp + expectP->leeway))
      return FdscJwtExpired;

    if ((numericDate(claimsP, "nbf", &nbf) == true) && (expectP->now + expectP->leeway < nbf))
      return FdscJwtExpired;
  }

  if (expectP->issuer != NULL)
  {
    CorNode* issP = corTreeLookup(claimsP, "iss");

    if ((issP == NULL) || (issP->type != CorString) || (strcmp(issP->value.s, expectP->issuer) != 0))
      return FdscJwtWrongIssuer;
  }

  if ((expectP->audience != NULL) && (audienceHas(claimsP, expectP->audience) == false))
    return FdscJwtWrongAudience;

  *claimsPP = claimsP;
  return FdscJwtOk;
}



// -----------------------------------------------------------------------------
//
// fdscJwtCheckString -
//
const char* fdscJwtCheckString(FdscJwtCheck check)
{
  switch (check)
  {
  case FdscJwtOk:            return "ok";
  case FdscJwtMalformed:     return "not a JWT";
  case FdscJwtUnknownKey:    return "signed by an unknown key";
  case FdscJwtBadSignature:  return "invalid signature";
  case FdscJwtBadAlg:        return "algorithm not accepted";
  case FdscJwtExpired:       return "expired, or not yet valid";
  case FdscJwtWrongIssuer:   return "wrong issuer";
  case FdscJwtWrongAudience: return "wrong audience";
  }

  return "invalid";
}
