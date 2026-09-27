//
// FILE            adminVersion.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <sys/types.h>                            // open
#include <sys/stat.h>                             // open
#include <fcntl.h>                                // open
#include <unistd.h>                               // close

#include "kbase/version.h"                        // KBASE_VERSION
#include "kalloc/version.h"                       // KALLOC_VERSION
#include "kalloc/KAlloc.h"                        // KAlloc
#include "ktrace/ktraceVersion.h"                  // KTRACE_VERSION
#include "khash/version.h"                        // KHASH_VERSION
#include "corTree/version.h"                      // CORTREE_VERSION
#include "corJson/version.h"                      // CORJSON_VERSION
#include "kargs/kargsVersion.h"                   // KARGS_VERSION
#include "corProm/version.h"                      // CORPROM_VERSION
#include "corRest/version.h"                       // CORREST_VERSION
#include "corRest/CorRestState.h"                   // corRest
#include "corJsonld/corJsonld.h"                    // CORJSONLD_VERSION
#include "corNgsild/corNgsild.h"                    // CORNGSILD_VERSION

#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd

#include "db/DbDriver.h"                         // db
#include "plugin/ApiPlugin.h"                     // apiPlugins, apiPluginCount

#include "api/admin/adminVersion.h"               // Own interface



// -----------------------------------------------------------------------------
//
// CORAINE_VERSION - injected from the broker's coraine.c via -D at compile time
//
#ifndef CORAINE_VERSION
#define CORAINE_VERSION "unknown"
#endif



// -----------------------------------------------------------------------------
//
// adminGetVersion -
//
bool adminGetVersion(void)
{
  KAlloc*  allocP = corRest.kallocP;
  CorNode* root   = corTreeObject(allocP, NULL);

  corTreeChildAdd(root, corTreeString(allocP, "coraine version", CORAINE_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "kbase",            KBASE_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "kalloc",           KALLOC_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "ktrace",           KTRACE_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "khash",            KHASH_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "corTree",          CORTREE_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "corJson",          CORJSON_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "kargs",            KARGS_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "corProm",          CORPROM_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "corRest",           CORREST_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "corJsonld",         CORJSONLD_VERSION));
  corTreeChildAdd(root, corTreeString(allocP, "corNgsild",         CORNGSILD_VERSION));

  //
  // DB plugin version info
  //
  if (db.versionInfo != NULL)
    db.versionInfo(&corRest.kalloc, root);

  //
  // API plugin version info
  //
  for (int i = 0; i < apiPluginCount; i++)
  {
    if (apiPlugins[i].versionInfo != NULL)
      apiPlugins[i].versionInfo(&corRest.kalloc, root);
  }

  //
  // Next free file descriptor - useful for detecting fd leaks
  //
  int fd = open("/etc/passwd", O_RDONLY);
  if (fd >= 0)
  {
    corTreeChildAdd(root, corTreeInteger(allocP, "Next File Descriptor", fd));
    close(fd);
  }

  corRest.out.responseTree = root;
  return true;
}
