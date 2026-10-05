#ifndef CORAINE_DBCHANGES_H_
#define CORAINE_DBCHANGES_H_

//
// FILE            dbChanges.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// What a write changed, for the store: one merge report per entity (LdMergeReport), handed to
// db.entityChangesApply / db.entityBulkChangesApply instead of the whole entity to entityReplace /
// entityBulkUpdate. A store then applies, logs and records in its history just what changed -
// mongoc a $set/$unset of those attributes, corDB a log record of them (ATTRS_PUT).
//
// A write that applies several fragments to one entity (a batch with the same id twice) folds their
// reports into one: every attribute any of them touched, its preValue the first one's - the
// attribute as it was before the write, which is what a store's history needs.
//

#include "corTree/CorNode.h"                         // CorNode
#include "corNgsild/ldEntityMerge.h"                 // LdMergeReport



// -----------------------------------------------------------------------------
//
// dbChangesAdd - one fragment's report folded into the entity's (dbReportP->changes NULL: the first)
//
extern void dbChangesAdd(LdMergeReport* dbReportP, LdMergeReport* fragReportP);



// -----------------------------------------------------------------------------
//
// dbChangesFinish - the reasons set from the entity after the write ("attributeDeleted" for an
// attribute it no longer has, "attributeModified" for the others), and "type" added: a fragment may
// add a type and nothing else, and a store applies what the report names
//
extern void dbChangesFinish(LdMergeReport* dbReportP, CorNode* entityP);

#endif  // CORAINE_DBCHANGES_H_
