//
// FILE            adminLogLines.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /admin/log/lines - the broker's recent log, from the lines it keeps in memory (--logLines, corLogRing),
// whatever --logTo is: the last 'lines' (100, at most 2000) of those of the 'types' (letters: E W I V T X),
// containing 'match', at or after 'since' (an ISO 8601 UTC time). Each line starts with its UTC time.
//
#include <stdlib.h>                               // free, strtol
#include <string.h>                               // strcmp, strstr, strchr, strlen

#include "corAlloc/CorAlloc.h"                    // CorAlloc
#include "corAlloc/corAlloc.h"                    // corAlloc
#include "corAlloc/corAllocStrdup.h"              // corAllocStrdup
#include "corLog/corLogRing.h"                    // corLogRingCopy
#include "corTree/corTreeBuilder.h"               // corTreeObject, corTreeArray, corTreeString, corTreeInteger, corTreeChildAdd
#include "corRest/CorRestState.h"                 // corRest
#include "corNgsild/corNgsild.h"                  // corNgsild, ldError, LD_ERROR_*

#include "api/admin/adminLogLines.h"              // Own interface



#define LINES_DEFAULT   100
#define LINES_MAX       2000
#define STAMP_LEN       24                        // 2026-11-20T12:00:01.123Z



// -----------------------------------------------------------------------------
//
// param - a URL parameter by name, NULL: not given
//
static const char* param(const char* name)
{
  for (int ix = 0; ix < corRest.in.uriParamCount; ix++)
  {
    if ((corRest.in.uriParamV[ix].key != NULL) && (strcmp(corRest.in.uriParamV[ix].key, name) == 0))
      return corRest.in.uriParamV[ix].value;
  }
  return NULL;
}



// -----------------------------------------------------------------------------
//
// adminGetLogLines -
//
bool adminGetLogLines(void)
{
  const char* linesP = param("lines");
  const char* types  = param("types");
  const char* match  = param("match");
  const char* since  = param("since");
  long        lines  = LINES_DEFAULT;
  char        from[STAMP_LEN + 1];

  if (linesP != NULL)
  {
    char* end;
    lines = strtol(linesP, &end, 10);
    if ((*end != 0) || (lines < 1) || (lines > LINES_MAX))
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid URL parameter", "lines: 1 to %d", LINES_MAX);
      return true;
    }
  }

  //
  // since: 2026-11-20T12:00:00Z or with milliseconds - compared as text with the lines' times, so the milliseconds
  // are made explicit
  //
  if (since != NULL)
  {
    size_t len = strlen(since);
    if ((len == 20) && (since[10] == 'T') && (since[19] == 'Z'))
    {
      memcpy(from, since, 19);
      memcpy(&from[19], ".000Z", 6);
    }
    else if ((len == STAMP_LEN) && (since[10] == 'T') && (since[19] == '.') && (since[23] == 'Z'))
      memcpy(from, since, STAMP_LEN + 1);
    else
    {
      ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid URL parameter", "since: a UTC time, 2026-11-20T12:00:00Z or 2026-11-20T12:00:00.000Z");
      return true;
    }
  }

  size_t len;
  char*  text = corLogRingCopy(&len);
  if (text == NULL)
  {
    ldError(409, LD_ERROR_CONFLICT, "No log kept in memory", "the broker runs with --logLines 0");
    return true;
  }

  //
  // The lines, newest last; the ones that pass the filters marked, and only the last 'lines' of them returned
  //
  CorAlloc* allocP = corRest.kallocP;
  int       total  = 0;
  int       passed = 0;
  char**    lineV  = (char**) corAlloc(allocP, sizeof(char*) * (len / (STAMP_LEN + 4) + 2));

  for (char* p = text; *p != 0; )
  {
    char* nl = strchr(p, '\n');
    if (nl != NULL)
      *nl = 0;
    ++total;

    bool ok = true;
    if ((since != NULL) && (strncmp(p, from, STAMP_LEN) < 0))
      ok = false;
    if (ok && (types != NULL) && ((strlen(p) <= STAMP_LEN + 1) || (strchr(types, p[STAMP_LEN + 1]) == NULL)))
      ok = false;
    if (ok && (match != NULL) && (strstr(p, match) == NULL))
      ok = false;
    if (ok)
      lineV[passed++] = p;

    if (nl == NULL)
      break;
    p = nl + 1;
  }

  CorNode* root  = corTreeObject(allocP, NULL);
  CorNode* listP = corTreeArray(allocP, "lines");
  int      first = (passed > lines) ? passed - (int) lines : 0;

  for (int ix = first; ix < passed; ix++)
    corTreeChildAdd(listP, corTreeString(allocP, NULL, corAllocStrdup(allocP, lineV[ix])));

  corTreeChildAdd(root, listP);
  corTreeChildAdd(root, corTreeInteger(allocP, "matched", passed));
  corTreeChildAdd(root, corTreeInteger(allocP, "kept", total));
  free(text);

  corRest.out.responseTree = root;
  corNgsild.rawResponse    = true;
  return true;
}
