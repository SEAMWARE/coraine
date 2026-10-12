//
// FILE            fdscKeys.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <pthread.h>                                 // pthread_rwlock_*
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // calloc, free
#include <string.h>                                  // strcmp, strlen, strncpy

#include <openssl/bn.h>                              // BN_bin2bn
#include <openssl/core_names.h>                      // OSSL_PKEY_PARAM_*
#include <openssl/ecdsa.h>                           // ECDSA_SIG
#include <openssl/evp.h>                             // EVP_PKEY, EVP_DigestVerify*
#include <openssl/param_build.h>                     // OSSL_PARAM_BLD

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corAlloc/corAllocBufferInit.h"             // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"            // corAllocBufferReset
#include "corJson/CorJson.h"                         // CorJson
#include "corJson/corJsonCreate.h"                   // corJsonCreate
#include "corJson/corJsonParse.h"                    // corJsonParse
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup

#include "fdscBase64.h"                              // fdscBase64UrlDecode
#include "fdscTokenCache.h"                          // fdscTokenCacheClear
#include "fdscKeys.h"                                // Own interface



// -----------------------------------------------------------------------------
//
// FdscKey / FdscKeySet
//
typedef enum FdscKty { FdscKtyRsa, FdscKtyEc } FdscKty;

typedef struct FdscKey
{
  char       kid[128];                               // "" when the JWK has none
  FdscKty    kty;
  EVP_PKEY*  pkeyP;
} FdscKey;

typedef struct FdscKeySet
{
  int      n;
  FdscKey  keyV[];
} FdscKeySet;

static pthread_rwlock_t  setLock = PTHREAD_RWLOCK_INITIALIZER;
static FdscKeySet*       setP    = NULL;
static char*             setText = NULL;             // the JWK Set the set in use was made from - the refresh thread's only



// -----------------------------------------------------------------------------
//
// member - a string member of a JWK, NULL: none
//
static const char* member(CorNode* jwkP, const char* name)
{
  CorNode* nodeP = corTreeLookup(jwkP, name);

  return ((nodeP != NULL) && (nodeP->type == CorString)) ? nodeP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// decode - a base64url member of a JWK into a buffer of its own (free it); NULL: absent or not base64url
//
static unsigned char* decode(CorNode* jwkP, const char* name, int* lenP)
{
  const char* text = member(jwkP, name);

  if (text == NULL)
    return NULL;

  int            len = strlen(text);
  unsigned char* buf = (unsigned char*) malloc(len * 3 / 4 + 3);

  if (buf == NULL)
    return NULL;

  *lenP = fdscBase64UrlDecode(text, len, buf);

  if (*lenP <= 0)
  {
    free(buf);
    return NULL;
  }

  return buf;
}



// -----------------------------------------------------------------------------
//
// pkeyFromParams - an EVP_PKEY of 'type' ("RSA", "EC") from its parameters
//
static EVP_PKEY* pkeyFromParams(const char* type, OSSL_PARAM_BLD* bldP)
{
  OSSL_PARAM*    paramV = OSSL_PARAM_BLD_to_param(bldP);
  EVP_PKEY_CTX*  ctxP   = EVP_PKEY_CTX_new_from_name(NULL, type, NULL);
  EVP_PKEY*      pkeyP  = NULL;

  if ((paramV != NULL) && (ctxP != NULL) && (EVP_PKEY_fromdata_init(ctxP) == 1))
  {
    if (EVP_PKEY_fromdata(ctxP, &pkeyP, EVP_PKEY_PUBLIC_KEY, paramV) != 1)
      pkeyP = NULL;
  }

  EVP_PKEY_CTX_free(ctxP);
  OSSL_PARAM_free(paramV);
  return pkeyP;
}



// -----------------------------------------------------------------------------
//
// rsaKey - an RSA public key from a JWK's n and e (RFC 7518 § 6.3.1)
//
static EVP_PKEY* rsaKey(CorNode* jwkP)
{
  int             nLen = 0;
  int             eLen = 0;
  unsigned char*  nBuf = decode(jwkP, "n", &nLen);
  unsigned char*  eBuf = decode(jwkP, "e", &eLen);
  EVP_PKEY*       keyP = NULL;

  if ((nBuf != NULL) && (eBuf != NULL))
  {
    BIGNUM*         nP   = BN_bin2bn(nBuf, nLen, NULL);
    BIGNUM*         eP   = BN_bin2bn(eBuf, eLen, NULL);
    OSSL_PARAM_BLD* bldP = OSSL_PARAM_BLD_new();

    if ((nP != NULL) && (eP != NULL) && (bldP != NULL) &&
        (OSSL_PARAM_BLD_push_BN(bldP, OSSL_PKEY_PARAM_RSA_N, nP) == 1) &&
        (OSSL_PARAM_BLD_push_BN(bldP, OSSL_PKEY_PARAM_RSA_E, eP) == 1))
      keyP = pkeyFromParams("RSA", bldP);

    OSSL_PARAM_BLD_free(bldP);
    BN_free(nP);
    BN_free(eP);
  }

  free(nBuf);
  free(eBuf);
  return keyP;
}



// -----------------------------------------------------------------------------
//
// ecKey - an EC P-256 public key from a JWK's x and y (RFC 7518 § 6.2.1) - the uncompressed point 04|x|y
//
static EVP_PKEY* ecKey(CorNode* jwkP)
{
  const char* crv = member(jwkP, "crv");

  if ((crv == NULL) || (strcmp(crv, "P-256") != 0))
    return NULL;

  int             xLen = 0;
  int             yLen = 0;
  unsigned char*  xBuf = decode(jwkP, "x", &xLen);
  unsigned char*  yBuf = decode(jwkP, "y", &yLen);
  EVP_PKEY*       keyP = NULL;

  if ((xBuf != NULL) && (yBuf != NULL) && (xLen == 32) && (yLen == 32))
  {
    unsigned char   point[65];
    OSSL_PARAM_BLD* bldP = OSSL_PARAM_BLD_new();

    point[0] = 0x04;
    memcpy(&point[1],  xBuf, 32);
    memcpy(&point[33], yBuf, 32);

    if ((bldP != NULL) &&
        (OSSL_PARAM_BLD_push_utf8_string(bldP, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) == 1) &&
        (OSSL_PARAM_BLD_push_octet_string(bldP, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)) == 1))
      keyP = pkeyFromParams("EC", bldP);

    OSSL_PARAM_BLD_free(bldP);
  }

  free(xBuf);
  free(yBuf);
  return keyP;
}



// -----------------------------------------------------------------------------
//
// setFree -
//
static void setFree(FdscKeySet* sP)
{
  if (sP == NULL)
    return;

  for (int i = 0; i < sP->n; i++)
    EVP_PKEY_free(sP->keyV[i].pkeyP);

  free(sP);
}



// -----------------------------------------------------------------------------
//
// fdscKeysLoad -
//
bool fdscKeysLoad(char* jwksText, char* errorBuf, int errorBufSize)
{
  //
  // The same JWK Set as the one in use (the refresh thread fetches it every --fdscRefresh): nothing to do -
  // and the tokens checked with it stay checked
  //
  if ((setText != NULL) && (strcmp(setText, jwksText) == 0))
    return true;

  char* textCopy = strdup(jwksText);                 // corJsonParse works in place

  if (textCopy == NULL)
  {
    snprintf(errorBuf, errorBufSize, "out of memory");
    return false;
  }

  CorAlloc  ka;
  char      buf[8 * 1024];
  CorJson   cj;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "fdsc JWK Set");
  corJsonCreate(&cj, &ka);

  CorNode* rootP = corJsonParse(&cj, jwksText);
  CorNode* keysP = (rootP != NULL) ? corTreeLookup(rootP, "keys") : NULL;

  if ((keysP == NULL) || (keysP->type != CorArray))
  {
    snprintf(errorBuf, errorBufSize, "not a JWK Set (no 'keys' array)");
    corAllocBufferReset(&ka, false);
    free(textCopy);
    return false;
  }

  int n = 0;
  for (CorNode* jwkP = keysP->value.head; jwkP != NULL; jwkP = jwkP->next)
    ++n;

  FdscKeySet* newP = (FdscKeySet*) calloc(1, sizeof(FdscKeySet) + n * sizeof(FdscKey));

  if (newP == NULL)
  {
    snprintf(errorBuf, errorBufSize, "out of memory");
    corAllocBufferReset(&ka, false);
    free(textCopy);
    return false;
  }

  for (CorNode* jwkP = keysP->value.head; jwkP != NULL; jwkP = jwkP->next)
  {
    if (jwkP->type != CorObject)
      continue;

    const char* kty = member(jwkP, "kty");
    const char* use = member(jwkP, "use");
    const char* kid = member(jwkP, "kid");

    if ((use != NULL) && (strcmp(use, "sig") != 0))  // an encryption key never verifies a signature
      continue;

    FdscKey* kP = &newP->keyV[newP->n];

    if ((kty != NULL) && (strcmp(kty, "RSA") == 0))
    {
      kP->kty   = FdscKtyRsa;
      kP->pkeyP = rsaKey(jwkP);
    }
    else if ((kty != NULL) && (strcmp(kty, "EC") == 0))
    {
      kP->kty   = FdscKtyEc;
      kP->pkeyP = ecKey(jwkP);
    }

    if (kP->pkeyP == NULL)                           // a kind it does not know, or not a valid one: skipped
      continue;

    if (kid != NULL)
      strncpy(kP->kid, kid, sizeof(kP->kid) - 1);

    newP->n += 1;
  }

  corAllocBufferReset(&ka, false);

  pthread_rwlock_wrlock(&setLock);
  FdscKeySet* oldP = setP;
  setP = newP;
  pthread_rwlock_unlock(&setLock);

  setFree(oldP);                                     // no reader left: they hold the read lock while they use it
  fdscTokenCacheClear();                             // a token is checked again, with the new keys

  free(setText);
  setText = textCopy;
  return true;
}



// -----------------------------------------------------------------------------
//
// sigCheck - RS256: PKCS#1 v1.5 over SHA-256; ES256: the raw r|s (RFC 7518 § 3.4) as the DER OpenSSL takes
//
static bool sigCheck(FdscKey* kP, const char* input, int inputLen, const unsigned char* sig, int sigLen)
{
  unsigned char*  der    = NULL;
  int             derLen = 0;

  if (kP->kty == FdscKtyEc)
  {
    if (sigLen != 64)
      return false;

    ECDSA_SIG* esP = ECDSA_SIG_new();
    BIGNUM*    rP  = BN_bin2bn(sig, 32, NULL);
    BIGNUM*    sP  = BN_bin2bn(&sig[32], 32, NULL);

    if ((esP == NULL) || (rP == NULL) || (sP == NULL) || (ECDSA_SIG_set0(esP, rP, sP) != 1))
    {
      BN_free(rP);
      BN_free(sP);
      ECDSA_SIG_free(esP);
      return false;
    }

    derLen = i2d_ECDSA_SIG(esP, &der);
    ECDSA_SIG_free(esP);

    if (derLen <= 0)
      return false;

    sig    = der;
    sigLen = derLen;
  }

  EVP_MD_CTX* mdP = EVP_MD_CTX_new();
  bool        ok  = (mdP != NULL) &&
                    (EVP_DigestVerifyInit(mdP, NULL, EVP_sha256(), NULL, kP->pkeyP) == 1) &&
                    (EVP_DigestVerify(mdP, sig, sigLen, (const unsigned char*) input, inputLen) == 1);

  EVP_MD_CTX_free(mdP);
  OPENSSL_free(der);
  return ok;
}



// -----------------------------------------------------------------------------
//
// fdscKeysVerify -
//
FdscKeyCheck fdscKeysVerify(const char* kid, const char* alg, const char* input, int inputLen, const unsigned char* sig, int sigLen)
{
  FdscKty kty;

  if (strcmp(alg, "RS256") == 0)
    kty = FdscKtyRsa;
  else if (strcmp(alg, "ES256") == 0)
    kty = FdscKtyEc;
  else
    return FdscKeyBadAlg;

  FdscKeyCheck answer = FdscKeyUnknown;

  pthread_rwlock_rdlock(&setLock);

  for (int i = 0; (setP != NULL) && (i < setP->n); i++)
  {
    FdscKey* kP = &setP->keyV[i];

    if ((kid != NULL) && (strcmp(kP->kid, kid) != 0))
      continue;

    if (kP->kty != kty)
    {
      if (kid != NULL)                               // the key named is of another kind: an alg confusion
      {
        answer = FdscKeyBadAlg;
        break;
      }
      continue;
    }

    if (sigCheck(kP, input, inputLen, sig, sigLen) == true)
    {
      answer = FdscKeyOk;
      break;
    }

    answer = FdscKeyBadSig;

    if (kid != NULL)
      break;
  }

  pthread_rwlock_unlock(&setLock);
  return answer;
}



// -----------------------------------------------------------------------------
//
// fdscKeysCount -
//
int fdscKeysCount(void)
{
  pthread_rwlock_rdlock(&setLock);
  int n = (setP != NULL) ? setP->n : 0;
  pthread_rwlock_unlock(&setLock);

  return n;
}



// -----------------------------------------------------------------------------
//
// fdscKeysRelease -
//
void fdscKeysRelease(void)
{
  pthread_rwlock_wrlock(&setLock);
  FdscKeySet* oldP = setP;
  setP = NULL;
  pthread_rwlock_unlock(&setLock);

  setFree(oldP);
  fdscTokenCacheClear();
  free(setText);
  setText = NULL;
}
