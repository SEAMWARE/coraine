//
// FILE            logTrim.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The log file kept in bounds (doc/log-file.md).
//
// A thread looks at the log file's disk once a second (statvfs). Full to --logTrimAt percent (the blocks reserved
// for root not counted, as df says it), the log is trimmed; and on request (POST /admin/log/trim). A trim:
//
//   1. renames the log file <log>.trimming and opens a fresh <log>, put in place of the log's descriptor under
//      corLog's mutex (dup2 - no writer ever sees a closed descriptor): the lines written from then on go there;
//   2. reads <log>.trimming (mapped) and decides what goes: the oldest lines first, by type - traces, then verbose,
//      FIXME, info, warnings - until the disk is under --logTrimTo percent or the log at half its size, whichever
//      comes first (a request: half its size). Errors and exits are never dropped, nor the newest --logTrimKeep lines;
//   3. merges repeated W/E lines of the same text (all after the time) into one, the last of them, with
//      " (<n> times, <first> - <last>)" at its end - the times are the lines' own, the broker's uptime;
//   4. writes what stays to <log>.tmp and what goes, gzipped, to <log>.trimmed-<UTC time>.gz (none with
//      --logTrimKeepFor 0) - and gives the space of what it has read back to the disk as it goes (a hole punched
//      behind it), so a log that filled its disk is trimmed in the little space left;
//   5. under corLog's mutex: appends to <log>.tmp the lines the fresh <log> got meanwhile, renames it <log> and puts
//      it in place of the log's descriptor. <log>.trimming is deleted;
//   6. says what it did in one I line that is always written, starting with "Log trimmed".
//
// The files of trimmed-off lines are kept until they are deleted (DELETE /admin/log/trimmed/<name>) or
// --logTrimKeepFor has passed.
//
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                                    // fallocate, FALLOC_FL_PUNCH_HOLE
#endif
#include <stdio.h>                                     // snprintf, rename
#include <stdlib.h>                                    // malloc, calloc, realloc, free
#include <string.h>                                    // memchr, memcmp, memcpy, strchr, strcmp, strncmp, strerror
#include <stdint.h>                                    // uint64_t, uint8_t
#include <stdbool.h>                                   // bool
#include <errno.h>                                     // errno, EINTR
#include <unistd.h>                                    // write, close, unlink, access, dup2
#include <fcntl.h>                                     // open, fallocate, FALLOC_FL_*
#include <dirent.h>                                    // opendir, readdir, closedir
#include <time.h>                                      // time, nanosleep, gmtime_r
#include <pthread.h>                                   // pthread_*
#include <sys/stat.h>                                  // stat, fstat
#include <sys/mman.h>                                  // mmap, munmap, madvise
#include <sys/statvfs.h>                               // statvfs

#include <zlib.h>                                      // gzopen, gzwrite, gzclose

#include "corBase/corSizeText.h"                       // corSizeText
#include "corLog/corLog.h"                             // COR_I_ALWAYS, COR_W, COR_E, COR_V
#include "corLog/corLogGlobals.h"                      // corLogFd, corLogMutex

#include "logTrim/logTrim.h"                           // Own interface



// -----------------------------------------------------------------------------
//
// State - the paths, set once by logTrimInit; the rest under stateMutex. trimMutex: one trim at a time
//
static char             logDir[256];
static char             logPath[328];                  // "": no log file
static char             logName[64];                   // coraine.log
static char             trimmingPath[344];
static char             tmpPath[344];
static pthread_mutex_t  trimMutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  stateMutex = PTHREAD_MUTEX_INITIALIZER;
static int              limitAt;
static int              limitTo;
static int              limitKeep;
static int              limitKeepFor;
static LogTrimLast      last;
static LogTrimFile*     fileV      = NULL;
static int              fileCount  = 0;
static int              fileSize   = 0;
static long long        nextDiskTrimSize = 0;          // a trim for the disk waits until the log has grown back to this



// -----------------------------------------------------------------------------
//
// diskStat - the log's disk: bytes used and bytes a process may still use (the blocks reserved for root not counted)
//
static bool diskStat(unsigned long long* usedP, unsigned long long* availP)
{
  struct statvfs s;

  if (statvfs(logDir, &s) != 0)
    return false;

  *usedP  = (unsigned long long) (s.f_blocks - s.f_bfree) * s.f_frsize;
  *availP = (unsigned long long) s.f_bavail * s.f_frsize;
  return true;
}



// -----------------------------------------------------------------------------
//
// diskPercent - % full as df's Use% says it: used / (used + available to a process), rounded up
//
static int diskPercent(void)
{
  unsigned long long used;
  unsigned long long avail;

  if (diskStat(&used, &avail) == false)
    return -1;

  if (used + avail == 0)
    return 0;

  return (int) ((used * 100 + (used + avail) - 1) / (used + avail));
}



// -----------------------------------------------------------------------------
//
// diskBytesOver - how many bytes the disk has over 'percent' full (negative: under it)
//
static long long diskBytesOver(int percent)
{
  unsigned long long used;
  unsigned long long avail;

  if (diskStat(&used, &avail) == false)
    return 0;

  return (long long) used - (long long) ((used + avail) * (unsigned long long) percent / 100);
}



// -----------------------------------------------------------------------------
//
// typeIx - the index of a line type in LOG_TRIM_TYPES, -1 for a type never dropped (E, X) or no type
//
static int typeIx(char type)
{
  if (type == 0)
    return -1;

  const char* p = strchr(LOG_TRIM_TYPES, type);

  return (p == NULL)? -1 : (int) (p - LOG_TRIM_TYPES);
}



// -----------------------------------------------------------------------------
//
// headLine - a line as corLogOut writes it starts "<type>: <uptime>: " - the uptime seconds, a dot, milliseconds
//
static bool headLine(const char* p, const char* end, int* uptimeLenP)
{
  if ((end - p < 4) || (p[0] < 'A') || (p[0] > 'Z') || (p[1] != ':') || (p[2] != ' '))
    return false;

  const char* q     = &p[3];
  const char* start = q;

  while ((q < end) && (*q >= '0') && (*q <= '9'))
    ++q;

  if ((q == start) || (q >= end) || (*q != '.'))
    return false;

  ++q;
  const char* ms = q;

  while ((q < end) && (*q >= '0') && (*q <= '9'))
    ++q;

  if ((q - ms != 3) || (q + 1 >= end) || (q[0] != ':') || (q[1] != ' '))
    return false;

  *uptimeLenP = (int) (q - start);
  return true;
}



// -----------------------------------------------------------------------------
//
// Entry - a log line: its head line and the lines after it that are not heads (a message with newlines in it).
// A line before the first head (no type) is an entry of its own, and kept
//
typedef struct Entry
{
  long long  start;
  long long  end;                                      // the byte after its last newline
  char       type;                                     // 0: not a line of corLogOut's
  long long  uptimeStart;
  int        uptimeLen;
  long long  textStart;                                // after "<type>: <uptime>: "
} Entry;



// -----------------------------------------------------------------------------
//
// entryNext - the entry that starts at 'off', false at the end
//
static bool entryNext(const char* base, long long size, long long off, Entry* eP)
{
  if (off >= size)
    return false;

  const char* end = base + size;
  int         uptimeLen;
  const char* nl;
  long long   next;

  eP->start = off;
  eP->type  = 0;

  if (headLine(&base[off], end, &uptimeLen) == true)
  {
    eP->type        = base[off];
    eP->uptimeStart = off + 3;
    eP->uptimeLen   = uptimeLen;
    eP->textStart   = off + 3 + uptimeLen + 2;
  }

  nl   = memchr(&base[off], '\n', size - off);
  next = (nl != NULL)? (nl - base) + 1 : size;

  if (eP->type != 0)
  {
    while ((next < size) && (headLine(&base[next], end, &uptimeLen) == false))
    {
      nl   = memchr(&base[next], '\n', size - next);
      next = (nl != NULL)? (nl - base) + 1 : size;
    }
  }

  eP->end = next;
  return true;
}



// -----------------------------------------------------------------------------
//
// Plan - what goes: the entries before 'keepFrom' of the types dropAll, and the oldest of type 'partial' up to
// 'partialBytes'
//
typedef struct Plan
{
  long long  keepFrom;
  bool       dropAll[sizeof(LOG_TRIM_TYPES) - 1];
  int        partial;                                  // -1: none
  long long  partialBytes;
} Plan;



// -----------------------------------------------------------------------------
//
// entryDropped - is the entry number 'ix' one that goes? *partialDoneP: the bytes of 'partial' dropped so far.
// A W/E line of a repeated text (the bit in 'repeated') never goes: the repeats are merged into one line
//
static bool entryDropped(Plan* planP, long long ix, Entry* eP, const uint8_t* repeated, long long* partialDoneP)
{
  if ((ix >= planP->keepFrom) || ((repeated[ix / 8] & (1 << (ix % 8))) != 0))   // a repeated W/E line: merged, never dropped
    return false;

  int tIx = typeIx(eP->type);

  if (tIx < 0)
    return false;

  if (planP->dropAll[tIx] == true)
    return true;

  if ((tIx == planP->partial) && (*partialDoneP < planP->partialBytes))
  {
    *partialDoneP += eP->end - eP->start;
    return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// Repeat - a W/E text met more than once: merged into its last line
//
typedef struct Repeat
{
  uint64_t   hash;                                     // 0: a free slot
  char       type;
  long long  textStart;                                // its first line's text - to tell two texts of one hash apart
  long long  textLen;
  long long  count;
  long long  lastIx;
  char       firstUptime[24];
} Repeat;



// -----------------------------------------------------------------------------
//
// textOf - an entry's text: after "<type>: <uptime>: ", without its last newline
//
static long long textLenOf(const char* base, Entry* eP)
{
  long long len = eP->end - eP->textStart;

  if ((len > 0) && (base[eP->end - 1] == '\n'))
    --len;

  return len;
}



// -----------------------------------------------------------------------------
//
// textHash - FNV-1a of the type and the text, never 0 (a free slot)
//
static uint64_t textHash(char type, const char* p, long long len)
{
  uint64_t h = 14695981039346656037ULL;

  h ^= (uint8_t) type;
  h *= 1099511628211ULL;

  for (long long i = 0; i < len; i++)
  {
    h ^= (uint8_t) p[i];
    h *= 1099511628211ULL;
  }

  return (h == 0)? 1 : h;
}



// -----------------------------------------------------------------------------
//
// RepeatTable - open addressing, a power of two slots, grown at half full
//
typedef struct RepeatTable
{
  Repeat*  slotV;
  size_t   slots;
  size_t   used;
} RepeatTable;



// -----------------------------------------------------------------------------
//
// repeatSlot - the slot of the text, or the free slot where it goes
//
static Repeat* repeatSlot(RepeatTable* tP, const char* base, uint64_t hash, char type, long long textStart, long long textLen)
{
  size_t i = (size_t) hash & (tP->slots - 1);

  while (tP->slotV[i].hash != 0)
  {
    Repeat* rP = &tP->slotV[i];

    if ((rP->hash == hash) && (rP->type == type) && (rP->textLen == textLen) && (memcmp(&base[rP->textStart], &base[textStart], textLen) == 0))
      return rP;

    i = (i + 1) & (tP->slots - 1);
  }

  return &tP->slotV[i];
}



// -----------------------------------------------------------------------------
//
// repeatGrow - twice the slots
//
static bool repeatGrow(RepeatTable* tP, const char* base)
{
  RepeatTable bigger = { calloc(tP->slots * 2, sizeof(Repeat)), tP->slots * 2, tP->used };

  if (bigger.slotV == NULL)
    return false;

  for (size_t i = 0; i < tP->slots; i++)
  {
    if (tP->slotV[i].hash != 0)
      *repeatSlot(&bigger, base, tP->slotV[i].hash, tP->slotV[i].type, tP->slotV[i].textStart, tP->slotV[i].textLen) = tP->slotV[i];
  }

  free(tP->slotV);
  *tP = bigger;
  return true;
}



// -----------------------------------------------------------------------------
//
// Out - a file written through a buffer; 'failed' at the first error, and nothing written after it
//
typedef struct Out
{
  int     fd;
  char    buf[256 * 1024];
  size_t  used;
  bool    failed;
  int     err;
} Out;



// -----------------------------------------------------------------------------
//
// writeAll - write(2) until all of it is written
//
static bool writeAll(int fd, const char* p, size_t n)
{
  while (n > 0)
  {
    ssize_t nb = write(fd, p, n);

    if (nb < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }

    p += nb;
    n -= (size_t) nb;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// outFlush -
//
static void outFlush(Out* oP)
{
  if ((oP->failed == false) && (oP->used > 0) && (writeAll(oP->fd, oP->buf, oP->used) == false))
  {
    oP->failed = true;
    oP->err    = errno;
  }

  oP->used = 0;
}



// -----------------------------------------------------------------------------
//
// outWrite -
//
static void outWrite(Out* oP, const char* p, size_t n)
{
  if (oP->failed == true)
    return;

  if (oP->used + n > sizeof(oP->buf))
    outFlush(oP);

  if (n > sizeof(oP->buf))
  {
    if (writeAll(oP->fd, p, n) == false)
    {
      oP->failed = true;
      oP->err    = errno;
    }
    return;
  }

  memcpy(&oP->buf[oP->used], p, n);
  oP->used += n;
}



// -----------------------------------------------------------------------------
//
// repeatByLastIx - qsort: the repeats in the order of their last lines
//
static int repeatByLastIx(const void* a, const void* b)
{
  long long ia = ((const Repeat*) a)->lastIx;
  long long ib = ((const Repeat*) b)->lastIx;

  return (ia < ib)? -1 : (ia > ib)? 1 : 0;
}



// -----------------------------------------------------------------------------
//
// fileAdd - a kept file of trimmed-off lines (under stateMutex)
//
static void fileAdd(const char* name, time_t t, const char* why, long long size, long long lines)
{
  if (fileCount == fileSize)
  {
    int          newSize = (fileSize == 0)? 8 : fileSize * 2;
    LogTrimFile* newV    = realloc(fileV, newSize * sizeof(LogTrimFile));

    if (newV == NULL)
      return;

    fileV    = newV;
    fileSize = newSize;
  }

  LogTrimFile* fP = &fileV[fileCount++];

  snprintf(fP->name, sizeof(fP->name), "%s", name);
  fP->time  = t;
  fP->why   = why;
  fP->size  = size;
  fP->lines = lines;
}



// -----------------------------------------------------------------------------
//
// fileRemove - the kept file number 'ix' out of the list (under stateMutex), and out of 'last' if it is its file
//
static void fileRemove(int ix)
{
  if (strcmp(last.file, fileV[ix].name) == 0)
    last.file[0] = 0;

  memmove(&fileV[ix], &fileV[ix + 1], (fileCount - ix - 1) * sizeof(LogTrimFile));
  --fileCount;
}



// -----------------------------------------------------------------------------
//
// trimmedName - a name for a new file of trimmed-off lines: <log>.trimmed-<UTC time>.gz, -2, -3 ... after the time
// when there is one of that second already
//
static void trimmedName(time_t t, char* name, int nameSize, char* path, int pathSize)
{
  struct tm tm;
  char      stamp[32];

  gmtime_r(&t, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &tm);

  for (int n = 1; n < 1000; n++)
  {
    if (n == 1)
      snprintf(name, nameSize, "%s.trimmed-%s.gz", logName, stamp);
    else
      snprintf(name, nameSize, "%s.trimmed-%s-%d.gz", logName, stamp, n);

    snprintf(path, pathSize, "%s/%s", logDir, name);

    if (access(path, F_OK) != 0)
      return;
  }
}



// -----------------------------------------------------------------------------
//
// trim - the log file trimmed (under trimMutex) - see the top of the file
//
static void trim(bool request, LogTrimLast* rP)
{
  int at;
  int to;
  int keep;
  int keepFor;

  logTrimLimitsGet(&at, &to, &keep, &keepFor);

  memset(rP, 0, sizeof(*rP));
  rP->done       = true;
  rP->time       = time(NULL);
  rP->why        = (request == true)? "request" : "disk";
  rP->diskBefore = diskPercent();

  //
  // 1. Renamed, and a fresh log file in its place
  //
  if (rename(logPath, trimmingPath) != 0)
  {
    snprintf(rP->error, sizeof(rP->error), "cannot rename %s: %s", logPath, strerror(errno));
    return;
  }

  int freshFd = open(logPath, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, 0664);

  if (freshFd < 0)
  {
    snprintf(rP->error, sizeof(rP->error), "cannot open a fresh %s: %s", logPath, strerror(errno));
    rename(trimmingPath, logPath);
    return;
  }

  pthread_mutex_lock(&corLogMutex);
  dup2(freshFd, corLogFd);
  pthread_mutex_unlock(&corLogMutex);
  close(freshFd);

  //
  // 2. The old one, read
  //
  int          fd   = open(trimmingPath, O_RDWR);
  struct stat  st;
  char*        base = NULL;
  long long    size = 0;

  if ((fd >= 0) && (fstat(fd, &st) == 0) && (st.st_size > 0))
  {
    size = st.st_size;
    base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED)
    {
      snprintf(rP->error, sizeof(rP->error), "cannot map %s: %s", trimmingPath, strerror(errno));
      base = NULL;
    }
    else
      madvise(base, size, MADV_SEQUENTIAL);
  }

  rP->sizeBefore = size;

  Out*         tmpP    = calloc(1, sizeof(Out));
  gzFile       gz      = NULL;
  char         gzName[112] = "";
  char         gzPath[400];
  long long    gzLines = 0;
  long long    doneOff = 0;                            // read up to here
  uint8_t*     mergedAway = NULL;
  uint8_t*     repeated   = NULL;
  Repeat*      repeatV = NULL;
  long long    repeats = 0;
  RepeatTable  table   = { NULL, 0, 0 };

  if (tmpP == NULL)
    snprintf(rP->error, sizeof(rP->error), "no memory for the trim");
  else
  {
    tmpP->fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (tmpP->fd < 0)
      snprintf(rP->error, sizeof(rP->error), "cannot open %s: %s", tmpPath, strerror(errno));
  }

  if ((base != NULL) && (rP->error[0] == 0))
  {
    //
    // How many entries - the newest 'keep' of them stay whatever they are
    //
    Entry      e;
    long long  total = 0;
    long long  bytes[sizeof(LOG_TRIM_TYPES) - 1] = { 0 };

    for (long long off = 0; entryNext(base, size, off, &e) == true; off = e.end)
      ++total;

    Plan plan;

    memset(&plan, 0, sizeof(plan));
    plan.keepFrom = (total > keep)? total - keep : 0;
    plan.partial  = -1;

    //
    // 3. First the merge, as it loses nothing: the W/E lines before the newest 'keep' met more than once - every
    //    one but the last of a text is merged away (mergedAway: a bit per entry)
    //
    mergedAway  = calloc((size_t) (total / 8 + 1), 1);
    repeated    = calloc((size_t) (total / 8 + 1), 1);
    table.slots = 1024;
    table.slotV = calloc(table.slots, sizeof(Repeat));

    if ((mergedAway == NULL) || (repeated == NULL) || (table.slotV == NULL))
      snprintf(rP->error, sizeof(rP->error), "no memory for the trim");

    long long ix = 0;
    for (long long off = 0; (rP->error[0] == 0) && (entryNext(base, size, off, &e) == true) && (ix < plan.keepFrom); off = e.end, ++ix)
    {
      if ((e.type != 'W') && (e.type != 'E'))
        continue;

      long long len  = textLenOf(base, &e);
      uint64_t  hash = textHash(e.type, &base[e.textStart], len);
      Repeat*   rP2  = repeatSlot(&table, base, hash, e.type, e.textStart, len);

      if (rP2->hash == 0)
      {
        rP2->hash      = hash;
        rP2->type      = e.type;
        rP2->textStart = e.textStart;
        rP2->textLen   = len;
        rP2->count     = 1;
        rP2->lastIx    = ix;
        snprintf(rP2->firstUptime, sizeof(rP2->firstUptime), "%.*s", e.uptimeLen, &base[e.uptimeStart]);

        if ((++table.used * 2 > table.slots) && (repeatGrow(&table, base) == false))
          snprintf(rP->error, sizeof(rP->error), "no memory for the trim");
      }
      else
      {
        mergedAway[rP2->lastIx / 8] |= (uint8_t) (1 << (rP2->lastIx % 8));
        rP2->count  += 1;
        rP2->lastIx  = ix;
      }
    }

    //
    // What the merge saves, the lines of a repeated text (never dropped - they are one line already), and the bytes
    // of each type that can be dropped
    //
    long long saved = 0;

    ix = 0;
    for (long long off = 0; (rP->error[0] == 0) && (entryNext(base, size, off, &e) == true) && (ix < plan.keepFrom); off = e.end, ++ix)
    {
      if ((e.type == 'W') || (e.type == 'E'))
      {
        long long len = textLenOf(base, &e);
        Repeat*   rP2 = repeatSlot(&table, base, textHash(e.type, &base[e.textStart], len), e.type, e.textStart, len);

        if (rP2->count > 1)
        {
          repeated[ix / 8] |= (uint8_t) (1 << (ix % 8));

          if ((mergedAway[ix / 8] & (1 << (ix % 8))) != 0)
            saved += e.end - e.start;
          else
            saved -= 48;                               // the " (<n> times, <first> - <last>)" the last line gets
          continue;
        }
      }

      int tIx = typeIx(e.type);

      if (tIx >= 0)
        bytes[tIx] += e.end - e.start;
    }

    //
    // What goes: half the log - for the disk, what brings it under --logTrimTo if that is less - what the merge saves
    // first, then by type
    //
    long long target = size / 2;

    if (request == false)
    {
      long long over = diskBytesOver(to);

      if (over < target)
        target = (over > 0)? over : 0;
    }

    target -= (saved > 0)? saved : 0;

    for (int tIx = 0; (tIx < (int) sizeof(plan.dropAll)) && (target > 0); tIx++)
    {
      if (bytes[tIx] <= target)
      {
        plan.dropAll[tIx] = (bytes[tIx] > 0);
        target           -= bytes[tIx];
      }
      else
      {
        plan.partial      = tIx;
        plan.partialBytes = target;
        target            = 0;
      }
    }

    long long partialDone = 0;

    //
    // The repeated ones, in the order of their last lines - where each is written
    //
    if (rP->error[0] == 0)
    {
      repeatV = malloc((table.used + 1) * sizeof(Repeat));

      if (repeatV == NULL)
        snprintf(rP->error, sizeof(rP->error), "no memory for the trim");
      else
      {
        for (size_t i = 0; i < table.slots; i++)
        {
          if ((table.slotV[i].hash != 0) && (table.slotV[i].count > 1))
            repeatV[repeats++] = table.slotV[i];
        }

        qsort(repeatV, repeats, sizeof(Repeat), repeatByLastIx);
      }
    }

    //
    // 4. Written: what stays to <log>.tmp, what goes to the gzipped file - the space of what is read given back as it goes
    //
    if ((rP->error[0] == 0) && (keepFor > 0))
    {
      trimmedName(rP->time, gzName, sizeof(gzName), gzPath, sizeof(gzPath));
      gz = gzopen(gzPath, "wb6");

      if (gz == NULL)
        COR_W("cannot open %s for the trimmed-off log lines: %s - they are dropped", gzPath, strerror(errno));
      else
        gzbuffer(gz, 256 * 1024);
    }

    long long punched = 0;
    long long repeatIx = 0;

    partialDone = 0;
    ix          = 0;

    for (long long off = 0; (rP->error[0] == 0) && (entryNext(base, size, off, &e) == true); off = e.end, ++ix)
    {
      int  tIx     = typeIx(e.type);
      bool dropped = entryDropped(&plan, ix, &e, repeated, &partialDone);
      bool merged  = (dropped == false) && (ix < plan.keepFrom) && ((mergedAway[ix / 8] & (1 << (ix % 8))) != 0);

      if ((dropped == true) || (merged == true))
      {
        if (dropped == true)
          rP->dropped[tIx] += 1;
        else
          rP->merged += 1;

        if (gz != NULL)
        {
          gzwrite(gz, &base[e.start], (unsigned int) (e.end - e.start));
          ++gzLines;
        }
      }
      else if ((repeatIx < repeats) && (repeatV[repeatIx].lastIx == ix))
      {
        //
        // The last line of a repeated text: " (<n> times, <first> - <last>)" at its end
        //
        Repeat*   rP2 = &repeatV[repeatIx++];
        long long len = (e.end > e.start) && (base[e.end - 1] == '\n')? e.end - e.start - 1 : e.end - e.start;
        char      suffix[96];
        int       suffixLen;

        suffixLen = snprintf(suffix, sizeof(suffix), " (%lld times, %s - %.*s)\n", rP2->count, rP2->firstUptime, e.uptimeLen, &base[e.uptimeStart]);
        outWrite(tmpP, &base[e.start], (size_t) len);
        outWrite(tmpP, suffix, (size_t) suffixLen);
      }
      else
        outWrite(tmpP, &base[e.start], (size_t) (e.end - e.start));

      if (tmpP->failed == true)
      {
        snprintf(rP->error, sizeof(rP->error), "cannot write %s: %s", tmpPath, strerror(tmpP->err));
        break;
      }

      doneOff = e.end;

      //
      // Every 8 MiB read: what is written is flushed, and the space of what was read given back (a hole punched)
      //
      if (doneOff - punched >= 8 * 1024 * 1024)
      {
        outFlush(tmpP);
        if (tmpP->failed == true)
        {
          snprintf(rP->error, sizeof(rP->error), "cannot write %s: %s", tmpPath, strerror(tmpP->err));
          break;
        }

        long long upTo = doneOff & ~((long long) 4095);

        if ((upTo > punched) && (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, punched, upTo - punched) == 0))
          punched = upTo;
      }
    }

    outFlush(tmpP);
    if ((tmpP->failed == true) && (rP->error[0] == 0))
      snprintf(rP->error, sizeof(rP->error), "cannot write %s: %s", tmpPath, strerror(tmpP->err));
  }

  if (gz != NULL)
  {
    if ((gzclose(gz) != Z_OK) || (gzLines == 0) || (rP->error[0] != 0))
    {
      unlink(gzPath);
      gzName[0] = 0;
    }
  }

  //
  // 5. What stays in place of the log: the lines written meanwhile appended, under corLog's mutex
  //
  if ((tmpP != NULL) && (tmpP->fd >= 0) && (rP->error[0] == 0))
  {
    pthread_mutex_lock(&corLogMutex);

    int inFd = open(logPath, O_RDONLY);

    if (inFd >= 0)
    {
      ssize_t nb;

      while ((nb = read(inFd, tmpP->buf, sizeof(tmpP->buf))) > 0)
      {
        if (writeAll(tmpP->fd, tmpP->buf, (size_t) nb) == false)
        {
          snprintf(rP->error, sizeof(rP->error), "cannot write %s: %s", tmpPath, strerror(errno));
          break;
        }
      }

      close(inFd);
    }

    if (rP->error[0] == 0)
    {
      if (rename(tmpPath, logPath) != 0)
        snprintf(rP->error, sizeof(rP->error), "cannot rename %s: %s", tmpPath, strerror(errno));
      else
      {
        int newFd = open(logPath, O_WRONLY | O_APPEND);

        if (newFd >= 0)
        {
          dup2(newFd, corLogFd);
          close(newFd);
        }
      }
    }

    pthread_mutex_unlock(&corLogMutex);
  }

  if ((tmpP != NULL) && (tmpP->fd >= 0))
    close(tmpP->fd);

  if (base != NULL)
    munmap(base, size);

  if (fd >= 0)
    close(fd);

  if (rP->error[0] == 0)
    unlink(trimmingPath);
  else
  {
    //
    // Nothing is thrown away on an error: the log is the fresh file, and what was read so far is in <log>.tmp,
    // the rest in <log>.trimming from the byte after it
    //
    char more[800];

    snprintf(more, sizeof(more), " - %s holds the log up to byte %lld of %s", tmpPath, doneOff, trimmingPath);
    strncat(rP->error, more, sizeof(rP->error) - strlen(rP->error) - 1);
  }

  if (table.slotV != NULL) free(table.slotV);
  if (repeatV     != NULL) free(repeatV);
  if (mergedAway  != NULL) free(mergedAway);
  if (repeated    != NULL) free(repeated);
  if (tmpP        != NULL) free(tmpP);

  struct stat after;

  rP->sizeAfter = (stat(logPath, &after) == 0)? after.st_size : 0;
  rP->diskAfter = diskPercent();

  if (gzName[0] != 0)
  {
    struct stat gst;

    snprintf(rP->file, sizeof(rP->file), "%s", gzName);

    pthread_mutex_lock(&stateMutex);
    fileAdd(gzName, rP->time, rP->why, (stat(gzPath, &gst) == 0)? gst.st_size : 0, gzLines);
    pthread_mutex_unlock(&stateMutex);
  }
}



// -----------------------------------------------------------------------------
//
// trimSay - the I line of a trim, always written: "Log trimmed (<why>): ..." - c3 knows a trim by its start
//
static void trimSay(LogTrimLast* rP, int at)
{
  if (rP->error[0] != 0)
  {
    COR_E("Log trim failed (%s): %s", rP->why, rP->error);
    return;
  }

  char why[48];
  char dropped[160] = "";
  int  len          = 0;
  char before[32];
  char after[32];
  char file[160]    = "";

  if (strcmp(rP->why, "disk") == 0)
    snprintf(why, sizeof(why), "disk %d%% >= %d%%", rP->diskBefore, at);
  else
    snprintf(why, sizeof(why), "request");

  for (int tIx = 0; tIx < (int) sizeof(LOG_TRIM_TYPES) - 1; tIx++)
  {
    if (rP->dropped[tIx] > 0)
      len += snprintf(&dropped[len], sizeof(dropped) - len, "%s%c %lld", (len == 0)? "" : ", ", LOG_TRIM_TYPES[tIx], rP->dropped[tIx]);
  }

  if (rP->file[0] != 0)
    snprintf(file, sizeof(file), "; the trimmed-off lines in %s", rP->file);

  COR_I_ALWAYS("Log trimmed (%s): dropped %s; %lld repeated lines merged; %s -> %s, the disk %d%% full%s",
               why,
               (len == 0)? "none" : dropped,
               rP->merged,
               corSizeText((uint64_t) rP->sizeBefore, before, sizeof(before)),
               corSizeText((uint64_t) rP->sizeAfter,  after,  sizeof(after)),
               rP->diskAfter,
               file);

  if ((strcmp(rP->why, "disk") == 0) && (at > 0) && (rP->diskAfter >= at))
    COR_W("the disk is still %d%% full after the log trim - other files than the log fill it", rP->diskAfter);
}



// -----------------------------------------------------------------------------
//
// trimRun - a trim, its result kept as the last one and said in the log
//
static void trimRun(bool request, LogTrimLast* resultP)
{
  LogTrimLast r;
  int         at;
  int         to;
  int         keep;
  int         keepFor;

  pthread_mutex_lock(&trimMutex);
  trim(request, &r);
  pthread_mutex_unlock(&trimMutex);

  logTrimLimitsGet(&at, &to, &keep, &keepFor);

  //
  // After a trim for the disk, the next one waits until the log has grown by a tenth (at least 1 MiB): a disk that
  // other files fill is not emptied by trimming the log again and again, every second
  //
  pthread_mutex_lock(&stateMutex);
  last = r;
  if (request == false)
    nextDiskTrimSize = r.sizeAfter + ((r.sizeAfter < 10 * 1024 * 1024)? 1024 * 1024 : r.sizeAfter / 10);
  pthread_mutex_unlock(&stateMutex);

  trimSay(&r, at);

  if (resultP != NULL)
    *resultP = r;
}



// -----------------------------------------------------------------------------
//
// filesExpire - the kept files past --logTrimKeepFor, deleted
//
static void filesExpire(void)
{
  time_t now = time(NULL);

  pthread_mutex_lock(&stateMutex);

  for (int ix = 0; ix < fileCount; ix++)
  {
    if (fileV[ix].time + limitKeepFor <= now)
    {
      char path[400];

      snprintf(path, sizeof(path), "%s/%s", logDir, fileV[ix].name);
      unlink(path);
      COR_V("trimmed-off log lines %s: --logTrimKeepFor passed - deleted", fileV[ix].name);
      fileRemove(ix);
      --ix;
    }
  }

  pthread_mutex_unlock(&stateMutex);
}



// -----------------------------------------------------------------------------
//
// logTrimThread - once a second: the kept files that have expired, and the disk
//
static void* logTrimThread(void* arg)
{
  (void) arg;

  while (true)
  {
    struct timespec second = { 1, 0 };

    nanosleep(&second, NULL);

    filesExpire();

    int at;
    int to;
    int keep;
    int keepFor;

    logTrimLimitsGet(&at, &to, &keep, &keepFor);
    if (at == 0)
      continue;

    int percent = diskPercent();
    if ((percent < 0) || (percent < at))
      continue;

    struct stat st;
    long long   next;

    pthread_mutex_lock(&stateMutex);
    next = nextDiskTrimSize;
    pthread_mutex_unlock(&stateMutex);

    if ((stat(logPath, &st) == 0) && (st.st_size >= next))
      trimRun(false, NULL);
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// logTrimInit -
//
bool logTrimInit(const char* dir, const char* progName, int at, int to, int keep, int keepFor)
{
  snprintf(logDir,       sizeof(logDir),       "%s", dir);
  snprintf(logName,      sizeof(logName),      "%s.log", progName);
  snprintf(logPath,      sizeof(logPath),      "%s/%s", logDir, logName);
  snprintf(trimmingPath, sizeof(trimmingPath), "%s.trimming", logPath);
  snprintf(tmpPath,      sizeof(tmpPath),      "%s.tmp", logPath);

  limitAt      = at;
  limitTo      = to;
  limitKeep    = keep;
  limitKeepFor = keepFor;

  //
  // What an earlier run left: its files of trimmed-off lines are kept (and expire) as this run's; a trim it did
  // not finish is said, its files left as they are
  //
  if (access(trimmingPath, F_OK) == 0)
    COR_W("a log trim of an earlier run did not finish: %s is left as it was", trimmingPath);
  if (access(tmpPath, F_OK) == 0)
    COR_W("a log trim of an earlier run did not finish: %s is left as it was", tmpPath);

  DIR* dirP = opendir(logDir);

  if (dirP != NULL)
  {
    struct dirent* entP;
    char           prefix[160];
    size_t         prefixLen = (size_t) snprintf(prefix, sizeof(prefix), "%s.trimmed-", logName);

    while ((entP = readdir(dirP)) != NULL)
    {
      size_t len = strlen(entP->d_name);

      if ((len > prefixLen + 3) && (strncmp(entP->d_name, prefix, prefixLen) == 0) && (strcmp(&entP->d_name[len - 3], ".gz") == 0) && (len < 112))
      {
        char        path[sizeof(logDir) + 300];
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s", logDir, entP->d_name);
        if (stat(path, &st) == 0)
          fileAdd(entP->d_name, st.st_mtime, NULL, st.st_size, -1);
      }
    }

    closedir(dirP);
  }

  pthread_t      tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  int r = pthread_create(&tid, &attr, logTrimThread, NULL);

  pthread_attr_destroy(&attr);

  if (r != 0)
  {
    COR_E("cannot start the log trim thread: %s", strerror(r));
    logPath[0] = 0;
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// logTrimPath -
//
const char* logTrimPath(void)
{
  return (logPath[0] == 0)? NULL : logPath;
}



// -----------------------------------------------------------------------------
//
// logTrimLimitsGet -
//
void logTrimLimitsGet(int* atP, int* toP, int* keepP, int* keepForP)
{
  pthread_mutex_lock(&stateMutex);
  *atP      = limitAt;
  *toP      = limitTo;
  *keepP    = limitKeep;
  *keepForP = limitKeepFor;
  pthread_mutex_unlock(&stateMutex);
}



// -----------------------------------------------------------------------------
//
// logTrimLimitsSet - -1: as it is. Refused (nothing changed): a 'to' that is not under 'at' (unless 'at' is 0)
//
const char* logTrimLimitsSet(int at, int to, int keep, int keepFor)
{
  pthread_mutex_lock(&stateMutex);

  int newAt = (at >= 0)? at : limitAt;
  int newTo = (to >= 0)? to : limitTo;

  if ((newAt > 0) && (newTo >= newAt))
  {
    pthread_mutex_unlock(&stateMutex);
    return "trimTo must be under trimAt";
  }

  limitAt = newAt;
  limitTo = newTo;

  if (keep    >= 0) limitKeep    = keep;
  if (keepFor >= 0) limitKeepFor = keepFor;

  pthread_mutex_unlock(&stateMutex);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// logTrimDisk -
//
int logTrimDisk(void)
{
  return (logPath[0] == 0)? -1 : diskPercent();
}



// -----------------------------------------------------------------------------
//
// logTrimRun -
//
bool logTrimRun(LogTrimLast* lastP)
{
  if (logPath[0] == 0)
    return false;

  trimRun(true, lastP);
  return true;
}



// -----------------------------------------------------------------------------
//
// logTrimLastGet -
//
void logTrimLastGet(LogTrimLast* lastP)
{
  pthread_mutex_lock(&stateMutex);
  *lastP = last;
  pthread_mutex_unlock(&stateMutex);
}



// -----------------------------------------------------------------------------
//
// logTrimFiles -
//
int logTrimFiles(LogTrimFile* outV, int max)
{
  pthread_mutex_lock(&stateMutex);

  int n = fileCount;

  for (int ix = 0; (ix < fileCount) && (ix < max); ix++)
    outV[ix] = fileV[ix];

  pthread_mutex_unlock(&stateMutex);
  return n;
}



// -----------------------------------------------------------------------------
//
// fileIx - the index of a kept file by its name, -1 for none (under stateMutex)
//
static int fileIx(const char* name)
{
  for (int ix = 0; ix < fileCount; ix++)
  {
    if (strcmp(fileV[ix].name, name) == 0)
      return ix;
  }

  return -1;
}



// -----------------------------------------------------------------------------
//
// logTrimFilePath -
//
bool logTrimFilePath(const char* name, char* path, int pathSize)
{
  pthread_mutex_lock(&stateMutex);

  bool found = (fileIx(name) >= 0);

  if (found == true)
    snprintf(path, pathSize, "%s/%s", logDir, name);

  pthread_mutex_unlock(&stateMutex);
  return found;
}



// -----------------------------------------------------------------------------
//
// logTrimFileDelete -
//
bool logTrimFileDelete(const char* name)
{
  pthread_mutex_lock(&stateMutex);

  int ix = fileIx(name);

  if (ix >= 0)
  {
    char path[400];

    snprintf(path, sizeof(path), "%s/%s", logDir, name);
    unlink(path);
    fileRemove(ix);
  }

  pthread_mutex_unlock(&stateMutex);
  return (ix >= 0);
}
