//
// FILE            channelConfigLoad.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                  // bool
#include <ctype.h>                                    // toupper
#include <string.h>                                   // strcmp
#include <stdio.h>                                    // fopen, fread, fclose
#include <stdlib.h>                                   // malloc, free
#include <unistd.h>                                   // access, R_OK

#include "kalloc/KAlloc.h"                            // KAlloc
#include "kalloc/kaBufferInit.h"                      // kaBufferInit
#include "kalloc/kaBufferReset.h"                     // kaBufferReset
#include "kjson/kjson.h"                              // Kjson
#include "kjson/kjBufferCreate.h"                     // kjBufferCreate
#include "kjson/kjParse.h"                            // kjParse
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/KjNode.h"                             // KjNode
#include "ktrace/kTrace.h"                            // KT_W, KT_X, KT_T

#include "corJsonld/corLdInit.h"                      // corLdCoreContext
#include "corNgsild/CorNgsild.h"                     // ldDefaultContext
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount

#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelCreate, CHANNEL_*
#include "bridge/bridgeDefaultEntity.h"               // bridgeDefaultEntitySet
#include "bridge/bridgeGoal.h"                        // bridgeGoalNotifyDefaultSet
#include "bridge/channelConfigLoad.h"                 // Own interface
#include "coraineTraceLevels.h"                       // KtBridge



// -----------------------------------------------------------------------------
//
// CHANNEL_CONFIG_DEFAULT - where the file lives when nobody says
//
#define CHANNEL_CONFIG_DEFAULT "/opt/seamware/etc/bridges.json"



// -----------------------------------------------------------------------------
//
// stringMember - a string-valued member, or NULL
//
static const char* stringMember(KjNode* objectP, const char* name)
{
  KjNode* nodeP = kjLookup(objectP, name);

  if ((nodeP == NULL) || (nodeP->type != KjString) || (nodeP->value.s == NULL) || (nodeP->value.s[0] == 0))
    return NULL;

  return nodeP->value.s;
}



// -----------------------------------------------------------------------------
//
// notificationParse - a default goal endpoint: { "endpoint": { "uri": ..., "accept": ... } }
//
// The subscription's own shape, so nothing new to learn and nothing to mint.
// Only http(s) - a goal's events go out as an HTTP notification - and accept
// application/json (the default) or application/ld+json.
//
// A default that cannot be used is WARNED ABOUT AND IGNORED, as an incomplete
// Channel entry is: its goals then fall back to the next default, or are polled,
// and a working deployment is not refused a start over where it notifies.
//
// @return true with *uriP (and *acceptP, NULL for the default) set
//
static bool notificationParse(const char* alias, const char* where, KjNode* notificationP, const char** uriP, const char** acceptP)
{
  KjNode*     endpointP = (notificationP->type == KjObject) ? kjLookup(notificationP, "endpoint") : NULL;
  const char* uri       = ((endpointP != NULL) && (endpointP->type == KjObject)) ? stringMember(endpointP, "uri")    : NULL;
  const char* accept    = ((endpointP != NULL) && (endpointP->type == KjObject)) ? stringMember(endpointP, "accept") : NULL;

  if ((uri == NULL) || ((strncmp(uri, "http://", 7) != 0) && (strncmp(uri, "https://", 8) != 0)))
  {
    KT_W("bridge '%s': %s - notification.endpoint.uri must be an http(s) URL - ignored", alias, where);
    return false;
  }

  if ((accept != NULL) && (strcmp(accept, "application/json") != 0) && (strcmp(accept, "application/ld+json") != 0))
  {
    KT_W("bridge '%s': %s - notification.endpoint.accept '%s' is neither application/json nor application/ld+json - ignored", alias, where, accept);
    return false;
  }

  *uriP    = uri;
  *acceptP = accept;

  return true;
}



// -----------------------------------------------------------------------------
//
// channelsLoad - one section of one bridge's configuration
//
// The topics, services and actions sections are the same three fields per
// entry - the endpoint's name, and the entity attribute it is bound to - and
// they differ only in what KIND of Channel they make and which way it faces.
// Those are parameters, not a reason for a second copy of this loop.
//
// Returns the number of Channels created. Does not return at all on a
// collision, which is deliberate - see channelConfigLoad below.
//
static int channelsLoad(const char*        alias,
                        KjNode*            sectionP,
                        BridgeChannelKind  kind,
                        BridgeDirection    direction,
                        Tenant*            tenantP,
                        KAlloc*            kaP)
{
  int created = 0;

  for (KjNode* entryP = sectionP->value.firstChildP; entryP != NULL; entryP = entryP->next)
  {
    const char* endpoint = entryP->name;

    if (entryP->type != KjObject)
    {
      KT_W("bridge '%s': entry '%s' is not an object - skipped", alias, endpoint);
      continue;
    }

    const char* entityId   = stringMember(entryP, "entityId");
    const char* entityType = stringMember(entryP, "entityType");
    const char* attribute  = stringMember(entryP, "attribute");

    //
    // An incomplete entry is WARNED ABOUT AND SKIPPED, not fatal.
    //
    // This is the one place the file's existing behaviour is kept as it is. A
    // half-written entry has always been ignored, deployments have files
    // carrying them, and a broker that refused to start over one would break
    // installations that are working today. A duplicate is a different matter
    // (below): it has never worked either, but nothing can be relying on it,
    // because the second entry was never reached.
    //
    if ((entityId == NULL) || (entityType == NULL) || (attribute == NULL))
    {
      KT_W("bridge '%s': endpoint '%s' is incomplete (entityId: %s, entityType: %s, attribute: %s) - skipped",
           alias, endpoint,
           (entityId   != NULL) ? entityId   : "-",
           (entityType != NULL) ? entityType : "-",
           (attribute  != NULL) ? attribute  : "-");
      continue;
    }

    //
    // Expanded ONCE, here, and not on every arriving sample.
    //
    // The file names attributes and types the short way, and everything past
    // this point - the store, subscription matching, the temporal record -
    // works in expanded names. The file carries no @context of its own, so the
    // core context is the whole of it, which for an ordinary name means @vocab.
    //
    // Note the return value is used rather than the CorLdItem: a term's own id
    // is not to be trusted on the core context.
    //
    // ⭐ ldDefaultContext, not corLdCoreContext. The file carries no @context
    // of its own and there is no request behind this - so if the deployment
    // was given a default user context, THAT is the user context here. The
    // mapping tool that writes this file writes that context beside it, and
    // the short names in one are terms of the other; expanding them with core
    // would give every one of them an @vocab IRI instead, and the same short
    // name arriving over HTTP would then land on a different attribute.
    //
    CorLdContext* ctxP           = ldDefaultContext(kaP);
    char*         attrExpanded   = corLdExpand(ctxP, attribute,  kaP, NULL, NULL);
    char*         typeExpanded   = corLdExpand(ctxP, entityType, kaP, NULL, NULL);

    if ((attrExpanded == NULL) || (typeExpanded == NULL))
    {
      KT_W("bridge '%s': endpoint '%s' - cannot expand '%s'/'%s' - skipped", alias, endpoint, entityType, attribute);
      continue;
    }

    Channel* clashP = NULL;

    //
    // Defaults, which are the loader's real work - the file names three things
    // per entry and a Channel has seven.
    //
    //   kind      whichever section this is
    //   direction both for a topic - a value arriving is stored, and a value
    //                  written locally goes back out on the same endpoint.
    //                  OUT for a service: the broker asks, and is never asked.
    //   retention mirror  - the broker holds what crosses. A Channel that held
    //                       nothing would be a relay, which this file has no
    //                       way of asking for.
    //
    int r = channelCreate(NULL,                       // no stored id: this Channel came from a file
                          alias,
                          endpoint,
                          kind,
                          direction,
                          ChannelRetentionMirror,
                          tenantP,
                          entityId,
                          typeExpanded,
                          attrExpanded,
                          &clashP);

    //
    // Only the attribute AS WRITTEN is named. The Channel already holding the
    // endpoint carries its attribute expanded, and printing one of each in the
    // same sentence reads like two unrelated problems to whoever has to fix it.
    //
    if (r == CHANNEL_DUP_ENDPOINT)
      KT_X(1, "bridge '%s': endpoint '%s' appears twice in the configuration - the second entry (%s/%s) could never be reached, the first already claims it",
           alias, endpoint, entityId, attribute);

    if (r == CHANNEL_BAD_INPUT)
      KT_X(1, "bridge '%s': endpoint '%s' is not a usable endpoint name", alias, endpoint);

    if (r == CHANNEL_DUP_TARGET)
      KT_X(1, "bridge '%s': endpoints '%s' and '%s' both write %s/%s - they would race, and the value would depend on which arrived last",
           alias, clashP->endpoint, endpoint, entityId, attribute);

    if (r != CHANNEL_OK)
      KT_X(1, "bridge '%s': endpoint '%s' could not be added (%d)", alias, endpoint, r);

    //
    // An action's default goal endpoint - where a goal that names none of its
    // own is notified. Only goals are notified this way: on a topic or a
    // service it would mean nothing, and saying so beats silently dropping it.
    //
    KjNode* notificationP = kjLookup(entryP, "notification");

    if ((notificationP != NULL) && (kind != BridgeChannelAction))
      KT_W("bridge '%s': endpoint '%s' - 'notification' is for actions only - ignored", alias, endpoint);
    else if (notificationP != NULL)
    {
      const char* uri    = NULL;
      const char* accept = NULL;
      Channel*    chP    = channelLookup(alias, endpoint);
      char        where[256];

      snprintf(where, sizeof(where), "endpoint '%s'", endpoint);

      if ((chP != NULL) && (notificationParse(alias, where, notificationP, &uri, &accept) == true))
      {
        chP->notifyUri    = strdup(uri);
        chP->notifyAccept = (accept != NULL) ? strdup(accept) : NULL;
      }
    }

    ++created;
  }

  return created;
}



// -----------------------------------------------------------------------------
//
// defaultEntityLoad - one bridge's catch-all, if it asked for one
//
// "defaultEntity": true, or an object naming the id and/or the type. Anything
// else in that member is a file saying something this broker does not
// understand, which is warned about rather than guessed at.
//
static void defaultEntityLoad(const char* alias, KjNode* nodeP, Tenant* tenantP, KAlloc* kaP)
{
  const char* entityId   = NULL;
  const char* entityType = NULL;

  if (nodeP->type == KjBoolean)
  {
    if (nodeP->value.b == false)
      return;
  }
  else if (nodeP->type == KjObject)
  {
    entityId   = stringMember(nodeP, "id");
    entityType = stringMember(nodeP, "type");
  }
  else
  {
    KT_W("bridge '%s': 'defaultEntity' is neither true/false nor an object - ignored", alias);
    return;
  }

  //
  // The alias uppercased, when the file did not say. "dds" becomes DDS, which
  // is the type the catch-all entity already carries elsewhere.
  //
  char derivedType[64];

  if (entityType == NULL)
  {
    size_t i = 0;

    for (; (alias[i] != 0) && (i < sizeof(derivedType) - 1); i++)
      derivedType[i] = toupper((unsigned char) alias[i]);
    derivedType[i] = 0;

    entityType = derivedType;
  }

  char* typeExpanded = corLdExpand(ldDefaultContext(kaP), entityType, kaP, NULL, NULL);

  if (typeExpanded == NULL)
  {
    KT_W("bridge '%s': cannot expand the defaultEntity type '%s' - no catch-all", alias, entityType);
    return;
  }

  if (bridgeDefaultEntitySet(alias, entityId, typeExpanded, tenantP) == false)
    KT_W("bridge '%s': the defaultEntity could not be set up - unclaimed endpoints will be dropped", alias);
}



// -----------------------------------------------------------------------------
//
// channelConfigLoad -
//
int channelConfigLoad(const char* path, bool explicitly, Tenant* tenantP)
{
  if (path == NULL)
    path = CHANNEL_CONFIG_DEFAULT;

  if (access(path, R_OK) != 0)
  {
    //
    // Asked for by name and not there: that is a mistake being made now, and
    // the person making it is at the keyboard. Found by default and not there
    // is not a mistake at all - a bridge may need no configuration.
    //
    if (explicitly == true)
      KT_X(1, "--bridgeConfig: cannot read '%s'", path);

    KT_T(KtBridge, "no bridge configuration at '%s' - no Channels from file", path);
    return 0;
  }

  //
  // fopen and not kFileRead: kFileRead takes a base and a relative path, and
  // given an empty base it does not resolve a plain relative path - so
  // --bridgeConfig etc/dds.json failed with "cannot read", one line after
  // access() had just said it was readable. Two answers about one file is worse
  // than either answer alone.
  //
  FILE* fP = fopen(path, "r");

  if (fP == NULL)
    KT_X(1, "cannot read the bridge configuration '%s'", path);

  fseek(fP, 0, SEEK_END);
  long fileSize = ftell(fP);
  fseek(fP, 0, SEEK_SET);

  if ((fileSize <= 0) || (fileSize > 4 * 1024 * 1024))
  {
    fclose(fP);
    KT_X(1, "the bridge configuration '%s' is empty or improbably large", path);
  }

  char* buf = (char*) malloc(fileSize + 1);

  if (buf == NULL)
  {
    fclose(fP);
    KT_X(1, "out of memory reading '%s'", path);
  }

  if (fread(buf, 1, (size_t) fileSize, fP) != (size_t) fileSize)
  {
    fclose(fP);
    free(buf);
    KT_X(1, "short read on the bridge configuration '%s'", path);
  }
  fclose(fP);
  buf[fileSize] = 0;

  //
  // A buffer of its own. Everything the cache keeps is copied into the
  // Channel, so nothing parsed here outlives this function, and the parse
  // does not have to share - or wait for - the broker's startup allocator.
  //
  char    kallocBuffer[8192];
  KAlloc  kalloc;
  Kjson   kjson;

  kaBufferInit(&kalloc, kallocBuffer, sizeof(kallocBuffer), 8 * 1024, NULL, "bridge config");

  Kjson*  kjP    = kjBufferCreate(&kjson, &kalloc);
  KjNode* treeP  = kjParse(kjP, buf);

  //
  // A file that is there but unusable is always fatal. It was put there on
  // purpose, and carrying on without it means running a configuration nobody
  // wrote.
  //
  if (treeP == NULL)
  {
    kaBufferReset(&kalloc, true);
    free(buf);
    KT_X(1, "the bridge configuration '%s' is not valid JSON", path);
  }

  int total = 0;

  //
  // The top-level key is the bridge alias, so one file describes every
  // transport a deployment runs, and a bridge that is not loaded is simply not
  // looked for.
  //
  for (int i = 0; i < bridgeCount; i++)
  {
    const char* alias = bridges[i].alias;

    if (alias == NULL)
      continue;

    KjNode* bridgeP = kjLookup(treeP, alias);
    if (bridgeP == NULL)
      continue;

    KjNode* ngsildP = kjLookup(bridgeP, "ngsild");
    if (ngsildP == NULL)
    {
      KT_W("bridge '%s' is named in '%s' but has no 'ngsild' section - no Channels from it", alias, path);
      continue;
    }

    //
    // The Bridge's default goal endpoint - for a goal whose request and Channel
    // name none. One address for every action Channel of this Bridge.
    //
    KjNode* bridgeNotificationP = kjLookup(ngsildP, "notification");

    if (bridgeNotificationP != NULL)
    {
      const char* uri    = NULL;
      const char* accept = NULL;

      if (notificationParse(alias, "its ngsild section", bridgeNotificationP, &uri, &accept) == true)
        bridgeGoalNotifyDefaultSet(alias, uri, accept);
    }

    KjNode* topicsP = kjLookup(ngsildP, "topics");
    if (topicsP != NULL)
      total += channelsLoad(alias, topicsP, BridgeChannelTopic, BridgeDirectionBoth, tenantP, &kalloc);

    //
    // ⭐ A service Channel faces OUT, and that is the client-only boundary
    // written down in the one place a deployment could otherwise contradict it.
    // The broker invokes a service; nothing on the domain invokes the broker,
    // because a context broker has no way to compute an answer. A file that
    // asked for the opposite would be asking for something that cannot exist.
    //
    KjNode* servicesP = kjLookup(ngsildP, "services");
    if (servicesP != NULL)
      total += channelsLoad(alias, servicesP, BridgeChannelService, BridgeDirectionOut, tenantP, &kalloc);

    //
    // An action Channel faces OUT for the same reason: the broker sends goals,
    // it cannot run one. Whatever comes back - feedback, status, a result - is
    // the other half of a goal the broker sent, not a sample.
    //
    KjNode* actionsP = kjLookup(ngsildP, "actions");
    if (actionsP != NULL)
      total += channelsLoad(alias, actionsP, BridgeChannelAction, BridgeDirectionOut, tenantP, &kalloc);

    //
    // And the catch-all, which is off unless the file asks for it.
    //
    KjNode* defaultEntityP = kjLookup(ngsildP, "defaultEntity");

    if (defaultEntityP != NULL)
      defaultEntityLoad(alias, defaultEntityP, tenantP, &kalloc);
  }

  kaBufferReset(&kalloc, true);
  free(buf);

  KT_T(KtBridge, "%d channel%s from '%s'", total, (total == 1) ? "" : "s", path);

  return total;
}
