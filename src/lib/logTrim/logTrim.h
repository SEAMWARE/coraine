#ifndef SRC_LIB_LOGTRIM_LOGTRIM_H_
#define SRC_LIB_LOGTRIM_LOGTRIM_H_

//
// FILE            logTrim.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                   // bool
#include <time.h>                                      // time_t



// -----------------------------------------------------------------------------
//
// LOG_TRIM_TYPES - the line types a trim drops, in the order it drops them: traces first, warnings last.
// E (errors) and X (exits) are never dropped
//
#define LOG_TRIM_TYPES "TVFIW"



// -----------------------------------------------------------------------------
//
// LogTrimLast - what a trim did
//
typedef struct LogTrimLast
{
  bool        done;                                    // a trim has run since the start
  time_t      time;
  const char* why;                                     // "disk" or "request"
  int         diskBefore;                              // % full, as df says it (the blocks reserved for root not counted)
  int         diskAfter;
  long long   dropped[sizeof(LOG_TRIM_TYPES) - 1];     // lines dropped, per type of LOG_TRIM_TYPES
  long long   merged;                                  // repeated W/E lines merged into one
  long long   sizeBefore;                              // bytes of the log file
  long long   sizeAfter;
  char        file[112];                               // the trimmed-off lines, gzipped - "" when none is kept
  char        error[1024];                              // "" - else what went wrong
} LogTrimLast;



// -----------------------------------------------------------------------------
//
// LogTrimFile - a file of trimmed-off lines, kept until it is deleted (DELETE /admin/log/trimmed/<name>) or
// --logTrimKeepFor has passed
//
typedef struct LogTrimFile
{
  char       name[112];                                // coraine.log.trimmed-20261127T181502Z.gz
  time_t     time;
  const char* why;                                     // "disk", "request" - NULL: found at the start, not known
  long long  size;
  long long  lines;                                    // -1: found at the start, not known
} LogTrimFile;



// -----------------------------------------------------------------------------
//
// logTrimInit - the log file <logDir>/<progName>.log is trimmed: by its own thread when the disk is full to
// 'at' percent (0: never), and on request (logTrimRun)
//
extern bool logTrimInit(const char* logDir, const char* progName, int at, int to, int keep, int keepFor);



// -----------------------------------------------------------------------------
//
// logTrimPath - the log file's path, NULL when there is none (the log goes to stdout)
//
extern const char* logTrimPath(void);



// -----------------------------------------------------------------------------
//
// logTrimLimitsGet/logTrimLimitsSet - the limits now; set: -1 leaves one as it is. NULL, or why not
//
extern void        logTrimLimitsGet(int* atP, int* toP, int* keepP, int* keepForP);
extern const char* logTrimLimitsSet(int at, int to, int keep, int keepFor);



// -----------------------------------------------------------------------------
//
// logTrimDisk - how full the log file's disk is, in % (as df says it), -1 when there is no log file
//
extern int logTrimDisk(void);



// -----------------------------------------------------------------------------
//
// logTrimRun - trim the log file now (a request). False when there is no log file
//
extern bool logTrimRun(LogTrimLast* lastP);



// -----------------------------------------------------------------------------
//
// logTrimLastGet - the last trim (lastP->done false: none since the start)
//
extern void logTrimLastGet(LogTrimLast* lastP);



// -----------------------------------------------------------------------------
//
// logTrimFiles - the kept files of trimmed-off lines, oldest first: how many there are, at most 'max' copied
//
extern int logTrimFiles(LogTrimFile* fileV, int max);



// -----------------------------------------------------------------------------
//
// logTrimFilePath - the path of a kept file, by its name - false for a name that is not one
//
extern bool logTrimFilePath(const char* name, char* path, int pathSize);



// -----------------------------------------------------------------------------
//
// logTrimFileDelete - delete a kept file - false for a name that is not one
//
extern bool logTrimFileDelete(const char* name);

#endif  // SRC_LIB_LOGTRIM_LOGTRIM_H_
