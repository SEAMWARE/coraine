#ifndef MONGOC_CONTEXT_H_
#define MONGOC_CONTEXT_H_

//
// FILE            mongocContext.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// JSON-LD context persistence (NGSI-LD § 5.13). Backed by a fixed
// "swBroker" database (collection "contexts"), independent of any tenant.
//

#include "kalloc/KAlloc.h"                            // KAlloc
#include "db/DbDriver.h"                              // DbContextRow



// -----------------------------------------------------------------------------
//
// mongocContextSave - upsert a context row by _id.
//
extern int mongocContextSave(const char* id, const char* url, int kind, const char* body);



// -----------------------------------------------------------------------------
//
// mongocContextDelete - remove a context row by _id.
//
extern int mongocContextDelete(const char* id);



// -----------------------------------------------------------------------------
//
// mongocContextList - load all rows; allocate the array in allocP.
//
extern int mongocContextList(KAlloc* allocP, DbContextRow** rowsPP, int* countP);

#endif
