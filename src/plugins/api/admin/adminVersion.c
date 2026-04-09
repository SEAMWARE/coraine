//
// FILE            adminVersion.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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
#include "swRest/version.h"                       // SWREST_VERSION
#include "swRest/SwRestState.h"                   // swRest
#include "swJsonld/swJsonld.h"                    // SWJSONLD_VERSION
#include "swNgsild/swNgsild.h"                    // SWNGSILD_VERSION

#include "kjson/kjBuilder.h"                      // kjObject, kjString, kjInteger, kjChildAdd

#include "db/DbDriver.h"                         // db
#include "plugin/ApiPlugin.h"                     // apiPlugins, apiPluginCount

#include "api/admin/adminVersion.h"               // Own interface



// -----------------------------------------------------------------------------
//
// SWBROKER_VERSION - injected from the broker's swBroker.c via -D at compile time
//
#ifndef SWBROKER_VERSION
#define SWBROKER_VERSION "unknown"
#endif



// -----------------------------------------------------------------------------
//
// adminGetVersion -
//
bool adminGetVersion(void)
{
  Kjson*   kjsonP = swRest.kjsonP;
  KjNode*  root   = kjObject(kjsonP, NULL);

  kjChildAdd(root, kjString(kjsonP, "swBroker version", SWBROKER_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kbase",            KBASE_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kalloc",           KALLOC_VERSION));
  kjChildAdd(root, kjString(kjsonP, "ktrace",           KTRACE_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kjson",            KJSON_VERSION));
  kjChildAdd(root, kjString(kjsonP, "kargs",            KARGS_VERSION));
  kjChildAdd(root, kjString(kjsonP, "swRest",           SWREST_VERSION));
  kjChildAdd(root, kjString(kjsonP, "swJsonld",         SWJSONLD_VERSION));
  kjChildAdd(root, kjString(kjsonP, "swNgsild",         SWNGSILD_VERSION));

  //
  // DB plugin version info
  //
  if (db.versionInfo != NULL)
    db.versionInfo(&swRest.kalloc, root);

  //
  // API plugin version info
  //
  for (int i = 0; i < apiPluginCount; i++)
  {
    if (apiPlugins[i].versionInfo != NULL)
      apiPlugins[i].versionInfo(&swRest.kalloc, root);
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

  swRest.out.responseTree = root;
  return true;
}
