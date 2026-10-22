#ifndef TROE_TROEQTREETOSQL_H_
#define TROE_TROEQTREETOSQL_H_

//
// FILE            troeQTreeToSql.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Compile an LdQNode tree (NGSI-LD ?q= expression, § 4.9) into a
// postgres-compatible WHERE-clause fragment for the troe_attrs table.
//
// v1 (small slice):
//   - LdQTermNode with comparison ops: == != > < >= <=
//   - Value types: number, string, bool
//   - Recursive AND / OR composition
//
// Not yet supported (return NULL → caller surfaces 501 / falls through):
//   - LdQPattern / LdQNotPattern (~=)
//   - LdQRange / LdQValueList / LdQDateTime
//   - LdQLinkedNode (sub-q over linked entities)
//   - Sub-attribute paths in attr (e.g. q=a.b>10)
//
// Output shape: EXISTS-correlated subqueries against troe_attrs
// keyed on the outer entity_id parameter ($1):
//
//   EXISTS (SELECT 1 FROM troe_attrs
//           WHERE entity_id = $1 AND attr_name = '<iri>' AND v_number > 10)
//
// Compose for AND/OR via "( ... AND ... )" / "( ... OR ... )".
//
// Every EXISTS ends in "/*W*/": the place for the window of the temporal
// query, which the store puts there (a comment if it does not).
//
// troeQTreeToRowSql: the same tree as a predicate on ONE row of troe_attrs -
// the instances that meet q (§ 11.3.3). A term on another attribute than the
// row's does not restrict it:
//
//   (attr_name <> '<iri>' OR (v_number > 10))
//

#include "corNgsild/LdQ.h"                                 // LdQNode
#include "corAlloc/CorAlloc.h"                            // CorAlloc


//
// Compile qTree → SQL WHERE fragment. Returns NULL when the tree uses
// an unsupported feature (caller falls back: skip the precondition).
//
extern const char* troeQTreeToSql(LdQNode* qTree, CorAlloc* allocP);



//
// Compile qTree → a predicate on one troe_attrs row. NULL as troeQTreeToSql.
//
extern const char* troeQTreeToRowSql(LdQNode* qTree, CorAlloc* allocP);



//
// TroeServedFunc - does an index of the store serve a value term on attr (an expanded IRI) in column
// (v_number, v_text)? NULL: no; else what the term's subquery adds to be the index's (its partial
// condition beyond the attribute - "" for none)
//
typedef const char* (*TroeServedFunc)(const char* attr, const char* column, void* servedData);



//
// troeQTreeToCandidateSql - the entities q can match, from the indexes of the store: an
// "entity_id IN (SELECT entity_id FROM troe_attrs WHERE <term>/*W*/)" per term an index serves (servedF),
// the AND of those of an AND, the OR of an OR whose every member has them. NULL: q has none - a query
// takes the entities as without. Only a narrowing: q itself still decides.
//
extern const char* troeQTreeToCandidateSql(LdQNode* qTree, TroeServedFunc servedF, void* servedData, CorAlloc* allocP);

#endif  // TROE_TROEQTREETOSQL_H_
