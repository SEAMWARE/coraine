#ifndef MONGOC_MONGOCINJECTTYPE_H_
#define MONGOC_MONGOCINJECTTYPE_H_

//
// FILE            mongocInjectType.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "kjson/KjNode.h"                             // KjNode



// -----------------------------------------------------------------------------
//
// mongocStripTypeDecouple - unlink the root-level "type" child from a
// tree, remembering the previous sibling so mongocStripTypeRestore can
// put it back at the exact same position.
//
// The caller uses this around mongocKjTreeToBson() for fixed-type
// records (Subscription, ContextSourceRegistration, ...) — the stored
// doc then doesn't carry the redundant JSON-LD constant, but the
// caller's tree is unchanged after restore.
//
// *typePOut is NULL if the tree has no root `type` child.
//
extern void mongocStripTypeDecouple(KjNode* treeP, KjNode** typePOut, KjNode** typePrevOut);



// -----------------------------------------------------------------------------
//
// mongocStripTypeRestore - put a previously decoupled node back at its
// original position (right after prevP, or at the head if prevP is NULL).
//
extern void mongocStripTypeRestore(KjNode* treeP, KjNode* typeP, KjNode* typePrevP);



// -----------------------------------------------------------------------------
//
// mongocInjectTypeAfterId - insert a freshly allocated "type":<val> right
// after the object's "id" (or at the head if no id), only if the object
// does not already have a "type" child.
//
// Used by the fixed-type-record retrieve paths (subscriptions, CSRs,
// CSR-subs) to put the JSON-LD-mandated constant back into the tree
// returned from the DB — the insert paths strip it so the stored doc
// doesn't carry the redundant constant.
//
extern void mongocInjectTypeAfterId(KjNode* objP, const char* typeValue);

#endif  // MONGOC_MONGOCINJECTTYPE_H_
