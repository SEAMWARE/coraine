#ifndef TROE_TROEQTREETOSQL_H_
#define TROE_TROEQTREETOSQL_H_

//
// FILE            troeQTreeToSql.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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

#include "corNgsild/LdQ.h"                                 // LdQNode
#include "kalloc/KAlloc.h"                                // KAlloc


//
// Compile qTree → SQL WHERE fragment. Returns NULL when the tree uses
// an unsupported feature (caller falls back: skip the precondition).
//
extern const char* troeQTreeToSql(LdQNode* qTree, KAlloc* allocP);

#endif  // TROE_TROEQTREETOSQL_H_
