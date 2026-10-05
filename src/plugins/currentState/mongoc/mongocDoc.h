#ifndef MONGOC_MONGOCDOC_H_
#define MONGOC_MONGOCDOC_H_

//
// FILE            mongocDoc.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The document store of DbDriver (docCreate ... docDelete): one mongo collection per kind, in the
// tenant's database, the document's id as _id.
//
#include "corTree/CorNode.h"                         // CorNode
#include "db/Tenant.h"                               // Tenant



extern int mongocDocCreate(Tenant* tenantP, const char* collection, const char* docId, CorNode* docP);
extern int mongocDocRetrieve(Tenant* tenantP, const char* collection, const char* docId, CorNode** docPP);
extern int mongocDocQuery(Tenant* tenantP, const char* collection, CorNode** arrayPP);
extern int mongocDocReplace(Tenant* tenantP, const char* collection, const char* docId, CorNode* docP);
extern int mongocDocDelete(Tenant* tenantP, const char* collection, const char* docId);

#endif  // MONGOC_MONGOCDOC_H_
