//
// FILE            channelConfigLoad.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdbool.h>                                  // bool
#include <string.h>                                   // strcmp
#include <unistd.h>                                   // access, R_OK

#include "kbase/kFileRead.h"                          // kFileRead
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
#include "corJsonld/corLdExpand.h"                    // corLdExpand

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, bridges, bridgeCount

#include "bridge/Channel.h"                           // Channel
#include "bridge/channelCache.h"                      // channelCreate, CHANNEL_*
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
// topicsLoad - one bridge's topics section
//
// Returns the number of Channels created. Does not return at all on a
// collision, which is deliberate - see channelConfigLoad below.
//
static int topicsLoad(const char* alias, KjNode* topicsP, Tenant* tenantP, KAlloc* kaP)
{
  int created = 0;

  for (KjNode* entryP = topicsP->value.firstChildP; entryP != NULL; entryP = entryP->next)
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
    CorLdContext* ctxP           = corLdCoreContext();
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
    //   kind      topic   - the only shape this section describes
    //   direction both    - a value arriving is stored, and a value written
    //                       locally goes back out on the same endpoint
    //   retention mirror  - the broker holds what crosses. A Channel that held
    //                       nothing would be a relay, which this file has no
    //                       way of asking for.
    //
    int r = channelCreate(NULL,                       // no stored id: this Channel came from a file
                          alias,
                          endpoint,
                          BridgeChannelTopic,
                          BridgeDirectionBoth,
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

    ++created;
  }

  return created;
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

  char* buf    = NULL;
  int   bufLen = 0;

  if (kFileRead((char*) "", (char*) path, &buf, &bufLen) != 0)
    KT_X(1, "cannot read the bridge configuration '%s'", path);

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

    KjNode* topicsP = kjLookup(ngsildP, "topics");
    if (topicsP != NULL)
      total += topicsLoad(alias, topicsP, tenantP, &kalloc);

    //
    // Services and actions are described by the same file and are not carried
    // yet. Saying so is the point: a deployment whose file has them would
    // otherwise see nothing happen and have no way to tell that from a quiet
    // peer.
    //
    if (kjLookup(ngsildP, "services") != NULL)
      KT_W("bridge '%s': the 'services' section is not carried yet and is being ignored", alias);

    if (kjLookup(ngsildP, "actions") != NULL)
      KT_W("bridge '%s': the 'actions' section is not carried yet and is being ignored", alias);
  }

  kaBufferReset(&kalloc, true);

  KT_T(KtBridge, "%d channel%s from '%s'", total, (total == 1) ? "" : "s", path);

  return total;
}
