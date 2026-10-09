/*
*
* FILE            sanPreload.c
*
* AUTHOR          Ken Zangelin
*
* Copyright 2026 Seamware
* SPDX-License-Identifier: Apache-2.0
*
* LD_PRELOADed into the broker of a sanitizer run (test/sanitizer/sanSetup.sh): dlclose() does nothing,
* so a plugin is still mapped when LeakSanitizer reports at exit and a leak inside a plugin names its
* frames. Not for any other use.
*/
int dlclose(void* handle)
{
  (void) handle;
  return 0;
}
