//
// FILE            migrateImport.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// coraine-import --file <stream> + the broker's store options: the WRITER half of a migration
// (doc/migration.md). The reader half - a tool per source - produces the stream; this writes it into
// whatever stores the importer was started with (-db mongoc|corDB, --troe timescale|corDB), through
// the broker's own code.
//
// The stream is one JSON object per line:
//
//   { "kind": "header", "format": "coraine-migration", "version": 1, ... }      (optional, first)
//   { "kind": "entity" | "subscription" | "registration" | "temporalEntity" | "temporalInstance",
//     "tenant": "<name>",            ("" or absent: the default tenant)
//     "data":   { ... } }
//
#include <stdio.h>                                        // FILE, fopen, getline, printf
#include <stdlib.h>                                       // free
#include <string.h>                                       // strcmp, memcpy, strlen
#include <time.h>                                         // clock_gettime

#include "corAlloc/corAlloc.h"                            // corAlloc
#include "corAlloc/corAllocBufferInit.h"                  // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"                 // corAllocBufferReset
#include "corTree/CorNode.h"                              // CorNode
#include "corTree/corTreeLookup.h"                        // corTreeLookup
#include "corJson/corJsonCreate.h"                        // corJsonCreate
#include "corJson/corJsonParse.h"                         // corJsonParse
#include "corRest/CorRestState.h"                         // corRest

#include "corNgsild/CorNgsild.h"                          // corNgsild

#include "db/Tenant.h"                                    // Tenant, tenant0, tenantGetOrCreate
#include "troe/TroeDriver.h"                              // troe

#include "migrate/MigrateState.h"                         // MigrateState
#include "migrate/migrateUtil.h"                          // migrateFail
#include "migrate/migrateEntity.h"                        // migrateEntity
#include "migrate/migrateApiObject.h"                     // migrateSubscription, migrateRegistration
#include "migrate/migrateHistory.h"                       // migrateTemporal*, migrateHistoryFlush, migrateHistoryCreatedRows
#include "migrate/migrateImport.h"                        // Own interface



// -----------------------------------------------------------------------------
//
// MIGRATE_FORMAT / MIGRATE_VERSION - what a header line must say
//
#define MIGRATE_FORMAT   "coraine-migration"
#define MIGRATE_VERSION  1



// -----------------------------------------------------------------------------
//
// nowNs -
//
static uint64_t nowNs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// kindFromString -
//
static int kindFromString(const char* kind)
{
  if (strcmp(kind, "entity")           == 0) return MigrateEntity;
  if (strcmp(kind, "subscription")     == 0) return MigrateSubscription;
  if (strcmp(kind, "registration")     == 0) return MigrateRegistration;
  if (strcmp(kind, "temporalEntity")   == 0) return MigrateTemporalEntity;
  if (strcmp(kind, "temporalInstance") == 0) return MigrateTemporalInstance;

  return -1;
}



// -----------------------------------------------------------------------------
//
// headerCheck - a header line: the format and a version this broker reads
//
static bool headerCheck(MigrateState* msP, CorNode* recP)
{
  CorNode* formatP  = corTreeLookup(recP, "format");
  CorNode* versionP = corTreeLookup(recP, "version");

  if ((formatP == NULL) || (formatP->type != CorString) || (strcmp(formatP->value.s, MIGRATE_FORMAT) != 0))
  {
    fprintf(stderr, "%s:%d: header: the format is not '%s'\n", msP->path, msP->lineNo, MIGRATE_FORMAT);
    return false;
  }

  if ((versionP == NULL) || (versionP->type != CorInt) || (versionP->value.i != MIGRATE_VERSION))
  {
    fprintf(stderr, "%s:%d: header: this broker reads version %d of the format\n", msP->path, msP->lineNo, MIGRATE_VERSION);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// recordImport - one line
//
static bool recordImport(MigrateState* msP, char* line)
{
  CorNode* recP = corJsonParse(corRest.corJsonP, line);

  if ((recP == NULL) || (recP->type != CorObject))
  {
    fprintf(stderr, "%s:%d: not a JSON object: %s\n", msP->path, msP->lineNo, corRest.corJsonP->errorString);
    return false;
  }

  CorNode* kindP   = corTreeLookup(recP, "kind");
  CorNode* tenantP = corTreeLookup(recP, "tenant");
  CorNode* dataP   = corTreeLookup(recP, "data");

  if ((kindP == NULL) || (kindP->type != CorString))
  {
    fprintf(stderr, "%s:%d: the record has no kind\n", msP->path, msP->lineNo);
    return false;
  }

  if (strcmp(kindP->value.s, "header") == 0)
    return headerCheck(msP, recP);

  int kind = kindFromString(kindP->value.s);
  if (kind < 0)
  {
    fprintf(stderr, "%s:%d: unknown kind '%s'\n", msP->path, msP->lineNo, kindP->value.s);
    return false;
  }

  const char* tenantName = ((tenantP != NULL) && (tenantP->type == CorString)) ? tenantP->value.s : "";
  Tenant*     tP         = (tenantName[0] == 0) ? &tenant0 : tenantGetOrCreate(tenantName);

  if (tP == NULL)
    return migrateFail(msP, (MigrateKind) kind, "the tenant '%s' could not be set up", tenantName);

  //
  // The history goes in order, and in batches: anything else that comes along first writes what
  // is pending (an entity's history before its current state, say, is the stream's order to keep)
  //
  bool history = ((kind == MigrateTemporalEntity) || (kind == MigrateTemporalInstance));

  if ((history == false) && (msP->pendingN > 0))
    migrateHistoryFlush(msP);

  corRest.requestStartTime = nowNs();

  switch ((MigrateKind) kind)
  {
  case MigrateEntity:            return migrateEntity(msP, tP, dataP);
  case MigrateSubscription:      return migrateSubscription(msP, tP, dataP);
  case MigrateRegistration:      return migrateRegistration(msP, tP, dataP);
  case MigrateTemporalEntity:    return migrateTemporalEntity(msP, tP, dataP);
  case MigrateTemporalInstance:  return migrateTemporalInstance(msP, tP, dataP);
  default:                       break;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// migrateImport -
//
int migrateImport(const char* path)
{
  MigrateState ms;

  memset(&ms, 0, sizeof(ms));
  ms.path = path;

  FILE* fP = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
  if (fP == NULL)
  {
    fprintf(stderr, "coraine-import: cannot open '%s'\n", path);
    return -1;
  }

  //
  // One arena for the whole run, reset between records - except while history events are
  // pending: they point into it until their batch is written. Allocated here, not static: a
  // broker that never imports does not carry a megabyte for it.
  //
  int   arenaSize   = 1024 * 1024;
  char* arenaBuffer = (char*) malloc(arenaSize);

  if (arenaBuffer == NULL)
  {
    fprintf(stderr, "coraine-import: out of memory\n");
    if (fP != stdin)
      fclose(fP);
    return -1;
  }

  corAllocBufferInit(&corRest.kalloc, arenaBuffer, arenaSize, arenaSize, NULL, "import");
  corRest.corJsonP = corJsonCreate(&corRest.corJson, &corRest.kalloc);
  corRest.kallocP  = &corRest.kalloc;

  char*   line     = NULL;
  size_t  lineSize = 0;
  ssize_t len;
  int     badLines = 0;

  while ((len = getline(&line, &lineSize, fP)) != -1)
  {
    ms.lineNo += 1;

    while ((len > 0) && ((line[len - 1] == '\n') || (line[len - 1] == '\r') || (line[len - 1] == ' ')))
      line[--len] = 0;

    if (len == 0)
      continue;

    if (ms.pendingN == 0)
      corAllocBufferReset(&corRest.kalloc, true);

    //
    // The tree points into the text it was parsed from, and a pending event into the tree:
    // the text goes into the arena too, never the getline buffer that the next line overwrites
    //
    char* text = corAlloc(&corRest.kalloc, len + 1);
    if (text == NULL)
    {
      fprintf(stderr, "%s:%d: out of memory\n", path, ms.lineNo);
      badLines += 1;
      continue;
    }
    memcpy(text, line, len + 1);

    int failedBefore = 0;
    for (int kind = 0; kind < MigrateKinds; kind++)
      failedBefore += ms.failedV[kind];

    if (recordImport(&ms, text) == false)
    {
      int failedAfter = 0;
      for (int kind = 0; kind < MigrateKinds; kind++)
        failedAfter += ms.failedV[kind];

      if (failedAfter == failedBefore)   // not a record of any kind: a line that is no record
        badLines += 1;
    }

    if (migrateHistoryBatchFull(&ms) == true)
      migrateHistoryFlush(&ms);
  }

  migrateHistoryFlush(&ms);
  migrateHistoryCreatedRows(&ms);

  free(line);
  if (fP != stdin)
    fclose(fP);

  static const char* kindNameV[MigrateKinds] = { "entities", "subscriptions", "registrations", "temporal entity events", "temporal instances" };

  int failed = badLines;

  fprintf(stderr, "import of %s:\n", path);
  for (int kind = 0; kind < MigrateKinds; kind++)
  {
    fprintf(stderr, "  %-24s %8d imported, %6d refused\n", kindNameV[kind], ms.okV[kind], ms.failedV[kind]);
    failed += ms.failedV[kind];
  }
  if ((ms.createdRowN > 0) || (ms.createdRowFailedN > 0))
  {
    fprintf(stderr, "  %-24s %8d written,  %6d failed\n", "created rows (history)", ms.createdRowN, ms.createdRowFailedN);
    failed += ms.createdRowFailedN;
  }
  if (badLines > 0)
    fprintf(stderr, "  %-24s %8d\n", "unreadable lines", badLines);

  fflush(stderr);

  return failed;
}
