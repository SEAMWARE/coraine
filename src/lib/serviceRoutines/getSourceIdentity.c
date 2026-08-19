//
// FILE            getSourceIdentity.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /info/sourceIdentity — return ContextSourceIdentity (§ 5.2.40) for
// this broker process. The contextSourceAlias is the per-tenant Via
// pseudonym, so peers can probe it once at CSR-registration time and use
// it for proactive loop detection during dispatch (§ 5.12 match rule:
// "no registration shall match if the CSourceRegistration
// contextSourceAlias can be found within the listing of previously
// encountered Context Sources").
//

#include <stddef.h>                                  // NULL
#include <string.h>                                  // strlen, strcpy, strcasecmp
#include <strings.h>                                 // strcasecmp
#include <stdio.h>                                   // snprintf
#include <time.h>                                    // time, gmtime_r, strftime

#include "corRest/CorRestState.h"                      // corRest
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjObject, kjString, kjChildAdd
#include "kjson/kjClone.h"                           // kjClone
#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corNgsild/corNgsild.h"                       // corNgsild, ldCsourceAliasBase, ldBrokerStartTimeSec
#include "corNgsild/ldCsourceAlias.h"                 // ldCsourceAliasForTenant

#include "coraineVersion.h"                         // CORAINE_VERSION (-Isrc/app/coraine)
#include "serviceRoutines/getSourceIdentity.h"       // Own interface



// -----------------------------------------------------------------------------
//
// nowIsoUtc - current time as ISO 8601 "YYYY-MM-DDTHH:MM:SSZ"
//
static void nowIsoUtc(char* buf, int bufSize)
{
  time_t    now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);
  strftime(buf, bufSize, "%Y-%m-%dT%H:%M:%SZ", &tm);
}



// -----------------------------------------------------------------------------
//
// uptimeIso - uptime as ISO 8601 duration "PT<sec>S" (seconds resolution)
//
// Format per clause 5.2.40 ("ISO 8601 format"). Seconds-only is a valid
// subset and avoids month/year ambiguity.
//
static void uptimeIso(char* buf, int bufSize)
{
  long long now     = (long long) time(NULL);
  long long uptimeS = now - ldBrokerStartTimeSec;
  if (uptimeS < 0) uptimeS = 0;
  snprintf(buf, bufSize, "PT%lldS", uptimeS);
}



// -----------------------------------------------------------------------------
//
// getSourceIdentity -
//
bool getSourceIdentity(void)
{
  //
  // Determine the per-tenant alias from the raw NGSILD-Tenant header.
  // This path is tenant-agnostic — the alias is derived algorithmically
  // from the header value, whether or not that tenant has been created
  // on this broker (see the tenantPreServiceHook bypass).
  //
  const char* tenant = NULL;
  for (int i = 0; i < corRest.in.httpHeaderCount; i++)
  {
    if (corRest.in.httpHeaderV[i].key == NULL) continue;
    if (strcasecmp(corRest.in.httpHeaderV[i].key, "NGSILD-Tenant") == 0)
    {
      tenant = corRest.in.httpHeaderV[i].value;
      break;
    }
  }

  const char* alias = ldCsourceAliasForTenant(tenant, &corRest.kalloc);
  if (alias == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "context source alias is not configured");
    return true;
  }

  //
  // Build the id as "urn:ngsi-ld:ContextSource:<alias>". The spec only
  // requires a valid URI uniquely identifying the source (+ tenant in the
  // multi-tenancy case) — reusing the alias guarantees uniqueness and
  // readability.
  //
  static const char idPrefix[] = "urn:ngsi-ld:ContextSource:";
  int   aliasLen = strlen(alias);
  char* idBuf    = (char*) kaAlloc(&corRest.kalloc, sizeof(idPrefix) + aliasLen);
  strcpy(idBuf, idPrefix);
  strcpy(idBuf + sizeof(idPrefix) - 1, alias);

  char uptimeBuf[32];
  char timeAtBuf[32];
  uptimeIso(uptimeBuf, sizeof(uptimeBuf));
  nowIsoUtc(timeAtBuf, sizeof(timeAtBuf));

  KjNode* body = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(body, kjString(corRest.kjsonP, "id",                   idBuf));
  kjChildAdd(body, kjString(corRest.kjsonP, "type",                 "ContextSourceIdentity"));
  kjChildAdd(body, kjString(corRest.kjsonP, "contextSourceAlias",   alias));
  // § 5.2.40 — broker product + version
  kjChildAdd(body, kjString(corRest.kjsonP, "contextSourceName",    "coraine"));
  kjChildAdd(body, kjString(corRest.kjsonP, "contextSourceVersion", CORAINE_VERSION));
  kjChildAdd(body, kjString(corRest.kjsonP, "contextSourceUptime",  uptimeBuf));
  kjChildAdd(body, kjString(corRest.kjsonP, "contextSourceTimeAt",  timeAtBuf));

  // § 5.2.40 contextSourceExtras — opaque JSON, never @context-expanded.
  // Cloned into the request arena so the response renderer doesn't mutate
  // the long-lived startup-parsed tree.
  if (ldContextSourceExtras != NULL)
  {
    KjNode* extras = kjClone(corRest.kjsonP, ldContextSourceExtras);
    if (extras != NULL)
    {
      extras->name = (char*) "contextSourceExtras";
      kjChildAdd(body, extras);
    }
  }

  corNgsild.rawResponse      = true;
  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
