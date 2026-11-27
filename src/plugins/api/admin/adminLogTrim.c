//
// FILE            adminLogTrim.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The log file kept in bounds, over the admin API (doc/log-file.md):
//
//   GET /admin/log                  + "file" (the log file, null: stdout) and "trim" (the limits, the disk, the last trim)
//   PUT/POST/PATCH /admin/log       + trimAt, trimTo, trimKeep, trimKeepFor - the limits of a running broker
//   POST /admin/log/trim            trim now - the answer: what the trim did
//   GET /admin/log/trimmed          the kept files of trimmed-off lines
//   GET /admin/log/trimmed/{name}   one of them, gzipped - Range: bytes=...
//   DELETE /admin/log/trimmed/{name}
//
#include <stdio.h>                                     // snprintf
#include <stdlib.h>                                    // strtoll
#include <string.h>                                    // strcmp, strncmp, strchr
#include <stdint.h>                                    // int64_t
#include <stdbool.h>                                   // bool
#include <errno.h>                                     // errno
#include <unistd.h>                                    // pread, close
#include <fcntl.h>                                     // open
#include <sys/stat.h>                                  // fstat

#include "corBase/corTimeIso.h"                        // corTimeIso
#include "corAlloc/corAlloc.h"                         // corAlloc
#include "corAlloc/corAllocStrdup.h"                   // corAllocStrdup
#include "corTree/corTreeBuilder.h"                    // corTreeObject, corTreeArray, corTreeString, corTreeInteger, ...
#include "corRest/CorRestState.h"                      // corRest
#include "corRest/corRestInHeader.h"                   // corRestInHeader
#include "corRest/corRestOutHeader.h"                  // corRestOutHeaderAdd
#include "corNgsild/corNgsild.h"                       // corNgsild
#include "corNgsild/ldError.h"                         // ldError, LD_ERROR_*

#include "logTrim/logTrim.h"                           // logTrim*, LogTrimLast, LogTrimFile

#include "api/admin/adminLogTrim.h"                    // Own interface



// -----------------------------------------------------------------------------
//
// paramLookup - a URL parameter's value, NULL when not given
//
static const char* paramLookup(const char* key)
{
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    if ((corRest.in.uriParamV[i].key != NULL) && (strcmp(corRest.in.uriParamV[i].key, key) == 0))
      return corRest.in.uriParamV[i].value;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// intParam - a URL parameter as an integer in [min, max]: -1 when not given, -2 when not such a number (said: 400)
//
static int intParam(const char* key, int min, int max)
{
  const char* s = paramLookup(key);

  if (s == NULL)
    return -1;

  char*     rest;
  long long v;

  errno = 0;
  v     = strtoll(s, &rest, 10);

  if ((s[0] == 0) || (*rest != 0) || (errno != 0) || (v < min) || (v > max))
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid URL parameter", "%s: %d to %d", key, min, max);
    return -2;
  }

  return (int) v;
}



// -----------------------------------------------------------------------------
//
// timeString - a time as ISO 8601 UTC, in the request's allocation
//
static const char* timeString(time_t t)
{
  char* buf = (char*) corAlloc(&corRest.kalloc, 40);

  corTimeIso((int64_t) t * 1000000000LL, 0, false, buf);
  return buf;
}



// -----------------------------------------------------------------------------
//
// lastTree - what a trim did: { time, why, diskBefore, diskAfter, dropped: { T, V, ... }, merged, sizeBefore,
// sizeAfter, file } - null for no trim since the start
//
static CorNode* lastTree(LogTrimLast* lP, const char* name)
{
  CorAlloc* allocP = corRest.kallocP;

  if (lP->done == false)
    return corTreeNull(allocP, name);

  CorNode* lastP    = corTreeObject(allocP, name);
  CorNode* droppedP = corTreeObject(allocP, "dropped");

  corTreeChildAdd(lastP, corTreeString(allocP,  "time",       timeString(lP->time)));
  corTreeChildAdd(lastP, corTreeString(allocP,  "why",        lP->why));
  corTreeChildAdd(lastP, corTreeInteger(allocP, "diskBefore", lP->diskBefore));
  corTreeChildAdd(lastP, corTreeInteger(allocP, "diskAfter",  lP->diskAfter));

  for (int tIx = 0; tIx < (int) sizeof(LOG_TRIM_TYPES) - 1; tIx++)
  {
    if (lP->dropped[tIx] > 0)
    {
      char* type = (char*) corAlloc(&corRest.kalloc, 2);

      type[0] = LOG_TRIM_TYPES[tIx];
      type[1] = 0;
      corTreeChildAdd(droppedP, corTreeInteger(allocP, type, lP->dropped[tIx]));
    }
  }

  corTreeChildAdd(lastP, droppedP);
  corTreeChildAdd(lastP, corTreeInteger(allocP, "merged",     lP->merged));
  corTreeChildAdd(lastP, corTreeInteger(allocP, "sizeBefore", lP->sizeBefore));
  corTreeChildAdd(lastP, corTreeInteger(allocP, "sizeAfter",  lP->sizeAfter));

  if (lP->file[0] != 0)
    corTreeChildAdd(lastP, corTreeString(allocP, "file", corAllocStrdup(&corRest.kalloc, lP->file)));
  else
    corTreeChildAdd(lastP, corTreeNull(allocP, "file"));

  if (lP->error[0] != 0)
    corTreeChildAdd(lastP, corTreeString(allocP, "error", corAllocStrdup(&corRest.kalloc, lP->error)));

  return lastP;
}



// -----------------------------------------------------------------------------
//
// adminLogTrimAdd -
//
void adminLogTrimAdd(CorNode* root)
{
  CorAlloc*    allocP = corRest.kallocP;
  const char*  path   = logTrimPath();
  CorNode*     trimP  = corTreeObject(allocP, "trim");
  int          at;
  int          to;
  int          keep;
  int          keepFor;
  LogTrimLast  last;

  if (path != NULL)
    corTreeChildAdd(root, corTreeString(allocP, "file", path));
  else
    corTreeChildAdd(root, corTreeNull(allocP, "file"));

  logTrimLimitsGet(&at, &to, &keep, &keepFor);
  logTrimLastGet(&last);

  corTreeChildAdd(trimP, corTreeInteger(allocP, "at",      at));
  corTreeChildAdd(trimP, corTreeInteger(allocP, "to",      to));
  corTreeChildAdd(trimP, corTreeInteger(allocP, "keep",    keep));
  corTreeChildAdd(trimP, corTreeInteger(allocP, "keepFor", keepFor));

  int disk = logTrimDisk();

  if (disk >= 0)
    corTreeChildAdd(trimP, corTreeInteger(allocP, "disk", disk));
  else
    corTreeChildAdd(trimP, corTreeNull(allocP, "disk"));

  corTreeChildAdd(trimP, lastTree(&last, "last"));
  corTreeChildAdd(root, trimP);
}



// -----------------------------------------------------------------------------
//
// adminLogTrimParams -
//
bool adminLogTrimParams(void)
{
  int at      = intParam("trimAt",      0, 100);
  if (at == -2) return false;
  int to      = intParam("trimTo",      1, 99);
  if (to == -2) return false;
  int keep    = intParam("trimKeep",    0, 10000000);
  if (keep == -2) return false;
  int keepFor = intParam("trimKeepFor", 0, 2592000);
  if (keepFor == -2) return false;

  const char* refused = logTrimLimitsSet(at, to, keep, keepFor);

  if (refused != NULL)
  {
    int nowAt;
    int nowTo;
    int nowKeep;
    int nowKeepFor;

    logTrimLimitsGet(&nowAt, &nowTo, &nowKeep, &nowKeepFor);
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid URL parameter", "%s (trimAt %d, trimTo %d)", refused, (at >= 0)? at : nowAt, (to >= 0)? to : nowTo);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// noLogFile - 409 for a broker that logs to stdout
//
static bool noLogFile(void)
{
  ldError(409, LD_ERROR_CONFLICT, "No log file", "the broker logs to stdout (--logTo stdout) - there is no file to trim");
  return true;
}



// -----------------------------------------------------------------------------
//
// adminPostLogTrim - POST /admin/log/trim
//
bool adminPostLogTrim(void)
{
  LogTrimLast last;

  if (logTrimRun(&last) == false)
    return noLogFile();

  corRest.out.responseTree = lastTree(&last, NULL);
  corNgsild.rawResponse    = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// adminGetLogTrimmed - GET /admin/log/trimmed: [ { name, time, why, size, lines, expires } ], oldest first
//
bool adminGetLogTrimmed(void)
{
  CorAlloc*    allocP = corRest.kallocP;
  CorNode*     arrayP = corTreeArray(allocP, NULL);
  int          n      = logTrimFiles(NULL, 0);
  LogTrimFile* fileV  = (LogTrimFile*) corAlloc(&corRest.kalloc, (n + 1) * sizeof(LogTrimFile));
  int          at;
  int          to;
  int          keep;
  int          keepFor;

  logTrimLimitsGet(&at, &to, &keep, &keepFor);
  n = logTrimFiles(fileV, n);

  for (int ix = 0; ix < n; ix++)
  {
    LogTrimFile* fP = &fileV[ix];
    CorNode*     oP = corTreeObject(allocP, NULL);

    corTreeChildAdd(oP, corTreeString(allocP, "name", corAllocStrdup(&corRest.kalloc, fP->name)));
    corTreeChildAdd(oP, corTreeString(allocP, "time", timeString(fP->time)));

    if (fP->why != NULL)
      corTreeChildAdd(oP, corTreeString(allocP, "why", fP->why));
    else
      corTreeChildAdd(oP, corTreeNull(allocP, "why"));

    corTreeChildAdd(oP, corTreeInteger(allocP, "size", fP->size));

    if (fP->lines >= 0)
      corTreeChildAdd(oP, corTreeInteger(allocP, "lines", fP->lines));
    else
      corTreeChildAdd(oP, corTreeNull(allocP, "lines"));

    corTreeChildAdd(oP, corTreeString(allocP, "expires", timeString(fP->time + keepFor)));
    corTreeChildAdd(arrayP, oP);
  }

  corRest.out.responseTree = arrayP;
  corNgsild.rawResponse    = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// trimmedNotFound -
//
static bool trimmedNotFound(const char* name)
{
  ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Trimmed-off log lines not found", "no kept file '%s' (deleted, or --logTrimKeepFor passed)", name);
  return true;
}



// -----------------------------------------------------------------------------
//
// rangeParse - "bytes=<first>-<last>", "bytes=<first>-", "bytes=-<suffix>" of a file of 'size' bytes:
// 1 a range (*firstP, *lastP), 0 none (no header), -1 not satisfiable or not understood
//
static int rangeParse(const char* range, long long size, long long* firstP, long long* lastP)
{
  if (range == NULL)
    return 0;

  if (strncmp(range, "bytes=", 6) != 0)
    return -1;

  const char* s    = &range[6];
  const char* dash = strchr(s, '-');

  if ((dash == NULL) || (strchr(s, ',') != NULL))       // one range only
    return -1;

  char* rest;

  if (dash == s)                                       // the last N bytes
  {
    long long n = strtoll(&dash[1], &rest, 10);

    if ((*rest != 0) || (n <= 0) || (size == 0))
      return -1;

    *firstP = (n >= size)? 0 : size - n;
    *lastP  = size - 1;
    return 1;
  }

  *firstP = strtoll(s, &rest, 10);
  if ((rest != dash) || (*firstP < 0) || (*firstP >= size))
    return -1;

  if (dash[1] == 0)
    *lastP = size - 1;
  else
  {
    *lastP = strtoll(&dash[1], &rest, 10);
    if ((*rest != 0) || (*lastP < *firstP))
      return -1;
    if (*lastP >= size)
      *lastP = size - 1;
  }

  return 1;
}



// -----------------------------------------------------------------------------
//
// adminGetLogTrimmedFile - GET /admin/log/trimmed/{name} - application/gzip; a Range: 206 with its part
//
bool adminGetLogTrimmedFile(void)
{
  const char* name = corRest.in.wildcard[0];
  char        path[700];

  if ((name == NULL) || (logTrimFilePath(name, path, sizeof(path)) == false))
    return trimmedNotFound((name != NULL)? name : "");

  int         fd = open(path, O_RDONLY);
  struct stat st;

  if ((fd < 0) || (fstat(fd, &st) != 0))
  {
    if (fd >= 0)
      close(fd);
    return trimmedNotFound(name);
  }

  long long size  = st.st_size;
  long long first = 0;
  long long lastB = size - 1;
  int       range = rangeParse(corRestInHeader("Range"), size, &first, &lastB);
  char      contentRange[96];

  if (range < 0)
  {
    close(fd);
    snprintf(contentRange, sizeof(contentRange), "bytes */%lld", size);
    corRestOutHeaderAdd("Content-Range", corAllocStrdup(&corRest.kalloc, contentRange));
    ldError(416, LD_ERROR_BAD_REQUEST_DATA, "Range Not Satisfiable", "Range: bytes=<first>-<last>, bytes=<first>- or bytes=-<n>, one range, within the %lld bytes of '%s'", size, name);
    return true;
  }

  long long n = (size == 0)? 0 : lastB - first + 1;

  if (n > 1024LL * 1024 * 1024)
  {
    close(fd);
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Too big for one response", "'%s' is %lld bytes - fetch it in parts of at most 1 GiB (Range: bytes=<first>-<last>)", name, size);
    return true;
  }

  char*     buf  = (char*) corAlloc(&corRest.kalloc, n + 1);
  long long done = 0;

  while (done < n)
  {
    ssize_t nb = pread(fd, &buf[done], n - done, first + done);

    if (nb <= 0)
      break;

    done += nb;
  }

  close(fd);

  if (done != n)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "cannot read '%s'", name);
    return true;
  }

  char disposition[160];

  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
  corRestOutHeaderAdd("Accept-Ranges", "bytes");
  corRestOutHeaderAdd("Content-Disposition", corAllocStrdup(&corRest.kalloc, disposition));

  if (range == 1)
  {
    snprintf(contentRange, sizeof(contentRange), "bytes %lld-%lld/%lld", first, lastB, size);
    corRestOutHeaderAdd("Content-Range", corAllocStrdup(&corRest.kalloc, contentRange));
  }

  corRest.out.payload        = buf;
  corRest.out.payloadSize    = (int) n;
  corRest.out.contentType    = (char*) "application/gzip";
  corRest.out.httpStatusCode = (range == 1)? 206 : 200;
  corNgsild.rawResponse      = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// adminDeleteLogTrimmedFile - DELETE /admin/log/trimmed/{name} - 204, 404 when it is not kept (any more)
//
bool adminDeleteLogTrimmedFile(void)
{
  const char* name = corRest.in.wildcard[0];

  if ((name == NULL) || (logTrimFileDelete(name) == false))
    return trimmedNotFound((name != NULL)? name : "");

  corRest.out.httpStatusCode = 204;
  return true;
}
