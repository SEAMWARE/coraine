//
// FILE            ftClient.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Test notification receiver for functional tests.
// Accumulates all incoming requests (e.g. POST /notify) in a global array.
// Provides:
//   GET    /dump   - return all accumulated requests as text
//   DELETE /dump   - clear the accumulator
//   GET    /die    - exit the process
//   POST   /**     - accumulate the request (catch-all)
//
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "kargs/kargs.h"
#include "kargs/kargsBuiltins.h"
#include "kjson/KjNode.h"
#include "kjson/kjson.h"
#include "kjson/kjBuilder.h"
#include "kjson/kjRender.h"
#include "kjson/kjRenderSize.h"
#include "kjson/kjClone.h"
#include "kjson/kjFree.h"
#include "kalloc/kalloc.h"
#include "kalloc/kaAlloc.h"
#include "ktrace/kTrace.h"
#include "swRest/swRest.h"



// -----------------------------------------------------------------------------
//
// Globals
//
static KjNode* dumpArray     = NULL;     // accumulates requests (malloc allocator)
static int     dumpCount     = 0;



// -----------------------------------------------------------------------------
//
// Command line arguments
//
unsigned short ftPort   = 7701;
bool           ftFg     = true;

static KArg ftArgV[] =
{
  { "--port",       "-p",  KaUShort, _vp &ftPort, KaOpt, _vp 7701,  _vp 1, _vp 65535, "TCP port to listen on" },
  { "--foreground", "-fg", KaBool,   _vp &ftFg,   KaOpt, _vp KTRUE, _vp KFALSE, _vp KTRUE, "run in foreground" },
  KARGS_END
};



// -----------------------------------------------------------------------------
//
// dumpInit - create the accumulator array
//
static void dumpInit(void)
{
  dumpArray = kjArray(NULL, NULL);
  dumpCount = 0;
}



// -----------------------------------------------------------------------------
//
// dumpClear - free all accumulated entries and reset
//
static void dumpClear(void)
{
  if (dumpArray != NULL)
    kjFree(dumpArray);

  dumpInit();
}



// -----------------------------------------------------------------------------
//
// dumpAccumulate - record an incoming request
//
// Stores: { "verb": "POST", "url": "/notify", "payload": "..." }
// Uses malloc allocator (NULL) so entries persist across requests.
//
static void dumpAccumulate(void)
{
  //
  // All strings come from per-request allocators that will be freed.
  // Use kjString with NULL allocator (malloc) — it strdup's the value.
  //
  KjNode* entry = kjObject(NULL, NULL);

  kjChildAdd(entry, kjString(NULL, "verb",    swRest.in.verbString ? swRest.in.verbString : "?"));
  kjChildAdd(entry, kjString(NULL, "url",     swRest.in.urlPath    ? swRest.in.urlPath    : "?"));

  // Headers
  KjNode* hdrs = kjObject(NULL, "headers");
  for (int i = 0; i < swRest.in.httpHeaderCount; i++)
    kjChildAdd(hdrs, kjString(NULL, swRest.in.httpHeaderV[i].key, swRest.in.httpHeaderV[i].value));
  kjChildAdd(entry, hdrs);

  // URI params
  if (swRest.in.uriParamCount > 0)
  {
    KjNode* params = kjObject(NULL, "params");
    for (int i = 0; i < swRest.in.uriParamCount; i++)
      kjChildAdd(params, kjString(NULL, swRest.in.uriParamV[i].key, swRest.in.uriParamV[i].value));
    kjChildAdd(entry, params);
  }

  // Body — deep-clone the request tree since the original is per-request allocated
  if (swRest.in.requestTree != NULL)
  {
    KjNode* bodyClone = kjClone(NULL, swRest.in.requestTree);
    if (bodyClone != NULL)
    {
      bodyClone->name = (char*) "body";
      kjChildAdd(entry, bodyClone);
    }
  }
  else if (swRest.in.payload != NULL && swRest.in.payloadSize > 0)
    kjChildAdd(entry, kjString(NULL, "body", swRest.in.payload));

  kjChildAdd(dumpArray, entry);
  dumpCount++;
}



// -----------------------------------------------------------------------------
//
// getDump - GET /dump — return all accumulated requests as a JSON array
//
static bool getDump(void)
{
  if (dumpArray == NULL || dumpCount == 0)
  {
    // Return empty array
    swRest.out.payload     = (char*) "[]";
    swRest.out.payloadSize = 2;
    return true;
  }

  //
  // Render dumpArray to JSON.
  // Use a scratch buffer allocated via malloc so the output survives
  // beyond the per-request kalloc lifetime — swRest copies the response
  // body (MHD_RESPMEM_MUST_COPY), so we can free this buffer after
  // the call returns by using kaAlloc from the per-request allocator.
  //
  int   bufSize = kjFastRenderSize(dumpArray) + 1;
  char* buf     = (char*) kaAlloc(&swRest.kalloc, bufSize);

  kjFastRender(dumpArray, buf);

  swRest.out.payload     = buf;
  swRest.out.payloadSize = strlen(buf);

  return true;
}



// -----------------------------------------------------------------------------
//
// deleteDump - DELETE /dump — clear the accumulator, return 204
//
static bool deleteDump(void)
{
  dumpClear();
  swRest.out.httpStatusCode = 204;
  return true;
}



// -----------------------------------------------------------------------------
//
// getDie - GET /die — exit the process
//
static bool getDie(void)
{
  swRest.out.httpStatusCode = 200;
  swRest.out.payload        = (char*) "bye";
  swRest.out.payloadSize    = 3;

  // Schedule exit after the response is sent
  // (MHD_RESPMEM_MUST_COPY means swRest copies the buffer before we exit)
  _exit(0);

  return true;  // unreachable
}



// -----------------------------------------------------------------------------
//
// postAccumulate - POST /** — catch-all, accumulate the request, return 200
//
static bool postAccumulate(void)
{
  dumpAccumulate();
  KT_T(1, "POST %s received (total: %d)", swRest.in.urlPath, dumpCount);
  swRest.out.httpStatusCode = 200;
  return true;
}



// -----------------------------------------------------------------------------
//
// Service table
//
static SwRestServiceSimplified ftServices[] =
{
  { SwVerbGet,    "/dump",  getDump,         0 },
  { SwVerbDelete, "/dump",  deleteDump,      0 },
  { SwVerbGet,    "/die",   getDie,          0 },
  { SwVerbPost,   "/**",    postAccumulate,  0 },
};

static int ftServiceCount = sizeof(ftServices) / sizeof(ftServices[0]);



// -----------------------------------------------------------------------------
//
// main
//
int main(int argC, char* argV[])
{
  char* progName = strrchr(argV[0], '/');
  progName = (progName != NULL) ? progName + 1 : argV[0];

  KArgsStatus ks = kargsInit(progName, ftArgV, "FTCLIENT");
  if (ks != KargsOk)
  {
    fprintf(stderr, "kargsInit failed\n");
    return 1;
  }

  ks = kargsParse(argC, argV);
  if (ks != KargsOk)
  {
    fprintf(stderr, "kargsParse failed\n");
    return 1;
  }

  if (ktInit("ftClient", "/tmp", false, NULL, "0-255", kaBuiltinVerbose, kaBuiltinDebug, false) != 0)
  {
    fprintf(stderr, "ktInit failed\n");
    return 1;
  }

  signal(SIGINT,  SIG_DFL);
  signal(SIGTERM, SIG_DFL);

  dumpInit();

  swRestSetPrettySpaces(2);

  if (swRestInit(ftServices, ftServiceCount, ftPort, 2) != 0)
  {
    fprintf(stderr, "ftClient: swRestInit failed on port %u\n", ftPort);
    return 1;
  }

  KT_I("ftClient running on port %u", ftPort);

  while (1)
    pause();

  return 0;
}
