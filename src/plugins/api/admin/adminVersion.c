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
#include "ktrace/ktraceVersion.h"                  // KTRACE_VERSION
#include "kjson/version.h"                        // KJSON_VERSION
#include "kargs/kargsVersion.h"                   // KARGS_VERSION
#include "corRest/version.h"                       // CORREST_VERSION
#include "corRest/CorRestState.h"                   // corRest
#include "corJsonld/corJsonld.h"                    // CORJSONLD_VERSION
#include "corNgsild/corNgsild.h"                    // CORNGSILD_VERSION

#include "kjson/kjBuilder.h"                      // kjObject, kjString, kjInteger, kjChildAdd

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
  Kjson*   kjsonP = corRest.kjsonP;
  KjNode*  root   = kjObject(kjsonP, NULL);

  kjChildAdd(root, kjString(kjsonP, "coraine version", CORAINE_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kbase",            KBASE_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kalloc",           KALLOC_VERSION));
  kjChildAdd(root, kjString(kjsonP, "ktrace",           KTRACE_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kjson",            KJSON_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kargs",            KARGS_VERSION));
  kjChildAdd(root, kjString(kjsonP, "corRest",           CORREST_VERSION));
  kjChildAdd(root, kjString(kjsonP, "corJsonld",         CORJSONLD_VERSION));
  kjChildAdd(root, kjString(kjsonP, "corNgsild",         CORNGSILD_VERSION));

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
    kjChildAdd(root, kjInteger(kjsonP, "Next File Descriptor", fd));
    close(fd);
  }

  corRest.out.responseTree = root;
  return true;
}
