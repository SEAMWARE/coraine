/*
*
* FILE            sanPreload.c
*
* AUTHOR          Ken Zangelin
*
* Copyright 2026 Seamware
* SPDX-License-Identifier: Apache-2.0
*
* LD_PRELOADed into the broker of a sanitizer run (test/sanitizer/sanSetup.sh). Not for any other use.
*
*   dlclose()      does nothing: a plugin is still mapped when LeakSanitizer reports at exit, so a leak
*                  inside a plugin names its frames
*
*   the UBSan report path, from SAN_UBSAN_LOG_PATH: gcc links UBSan as a runtime of its own
*                  (libubsan), and inside an ASan process that runtime ignores UBSAN_OPTIONS' log_path
*                  and prints to stderr - the broker's stderr, which is the test's output when a test
*                  runs the broker with 2>&1. libubsan's own __sanitizer_set_report_path sends its
*                  reports to <path>.<pid> instead.
*/
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>



// -----------------------------------------------------------------------------
//
// dlclose -
//
int dlclose(void* handle)
{
  (void) handle;
  return 0;
}



// -----------------------------------------------------------------------------
//
// ubsanReportPath -
//
__attribute__((constructor)) static void ubsanReportPath(void)
{
  const char* path = getenv("SAN_UBSAN_LOG_PATH");

  if (path == NULL)
    return;

  void* ubsan = dlopen("libubsan.so.1", RTLD_NOLOAD | RTLD_LAZY);  // only if the process has it
  if (ubsan == NULL)
    return;

  void (*setPath)(const char*) = (void (*)(const char*)) dlsym(ubsan, "__sanitizer_set_report_path");
  if (setPath != NULL)
    setPath(path);
}
