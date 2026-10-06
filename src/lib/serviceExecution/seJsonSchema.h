#ifndef SRC_LIB_SERVICEEXECUTION_SEJSONSCHEMA_H_
#define SRC_LIB_SERVICEEXECUTION_SEJSONSCHEMA_H_

//
// FILE            seJsonSchema.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// seJsonSchemaCheck - does 'valueP' satisfy the JSON Schema 'schemaP'? false: why, in errBuf
//
// The subset a service's input needs: type (string, integer, number, boolean, object, array, null, or
// an array of them), properties, required, additionalProperties (false, or a schema), items, enum,
// minimum, maximum, exclusiveMinimum, exclusiveMaximum, minLength, maxLength, minItems, maxItems.
// A keyword outside it is ignored - as JSON Schema says of a keyword a validator does not know.
// A schema 'true' accepts everything, 'false' nothing.
//
extern bool seJsonSchemaCheck(CorNode* schemaP, CorNode* valueP, const char* path, char* errBuf, int errBufSize);

#endif  // SRC_LIB_SERVICEEXECUTION_SEJSONSCHEMA_H_
