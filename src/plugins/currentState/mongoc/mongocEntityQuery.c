//
// FILE            mongocEntityQuery.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <mongoc/mongoc.h>                           // mongoc_collection_t, mongoc_collection_find_with_opts
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // strtod
#include <string.h>                                  // strlen, strcmp

#include "ktrace/kTrace.h"                               // KT_E
#include "kjson/KjNode.h"                            // KjNode
#include "kjson/kjBuilder.h"                         // kjArray, kjChildAdd
#include "kjson/kjBufferCreate.h"                    // kjBufferCreate
#include "corRest/CorRestState.h"                      // corRest
#include "corNgsild/LdVocab.h"                        // LD_VOCAB_SCOPE, LD_VOCAB_CREATED_AT
#include "corNgsild/LdGeoRel.h"                      // LdGeoRel, LdGeoRelType
#include "corNgsild/LdQ.h"                           // LdQNode, LdQTermNode, ...
#include "corNgsild/LdScopeExpr.h"                    // LdScopeExpr
#include "corNgsild/ldScopeMatch.h"                    // ldScopeToRegex
#include "corNgsild/LdProblem.h"                       // LD_ERROR_BAD_REQUEST_DATA, LD_ERROR_INTERNAL_ERROR

#include "db/DbDriver.h"                             // DB_OK, DB_ERR
#include "currentState/mongoc/mongocDotEscape.h"                  // mongocEscapeDotsInKey
#include "currentState/mongoc/mongocBsonToKjTree.h"               // mongocBsonToKjTree
#include "currentState/mongoc/mongocEntityQuery.h"                // Own interface




// -----------------------------------------------------------------------------
//
// Shared state from mongocInit.c
//
extern mongoc_client_pool_t*  poolP;



// -----------------------------------------------------------------------------
//
// bsonInArray - append a $in array for a NULL-terminated string vector
//
static void bsonInArray(bson_t* filterP, const char* field, char** valV)
{
  bson_t in;
  bson_t array;

  bson_append_document_begin(filterP, field, -1, &in);
  bson_append_array_begin(&in, "$in", 3, &array);

  for (int ix = 0; valV[ix] != NULL; ix++)
  {
    char key[16];
    int  keyLen = snprintf(key, sizeof(key), "%d", ix);
    bson_append_utf8(&array, key, keyLen, valV[ix], -1);
  }

  bson_append_array_end(&in, &array);
  bson_append_document_end(filterP, &in);
}




// -----------------------------------------------------------------------------
//
// bsonAppendScopeFilter - build BSON filter for scopeQ expression
//
static void bsonAppendScopeFilter(bson_t* filterP, LdScopeExpr* scopeExpr)
{
  char        regexBuf[512];
  const char* scopeKey = mongocEscapeDotsInKey(LD_VOCAB_SCOPE);

  //
  // Simple case: one group with one pattern
  //
  if (scopeExpr->groupCount == 1 && scopeExpr->groupV[0].count == 1)
  {
    ldScopeToRegex(scopeExpr->groupV[0].scopeV[0], regexBuf, sizeof(regexBuf));

    bson_t regexDoc;
    bson_append_document_begin(filterP, scopeKey, -1, &regexDoc);
    bson_append_regex(&regexDoc, "$regex", 6, regexBuf, "");
    bson_append_document_end(filterP, &regexDoc);
    return;
  }

  //
  // Simple case: all groups have count==1 - pure OR, use $or with $regex
  //
  if (scopeExpr->isSimple)
  {
    bson_t orArray;
    bson_append_array_begin(filterP, "$or", 3, &orArray);

    for (int gix = 0; gix < scopeExpr->groupCount; gix++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", gix);

      ldScopeToRegex(scopeExpr->groupV[gix].scopeV[0], regexBuf, sizeof(regexBuf));

      bson_t orElem;
      bson_append_document_begin(&orArray, key, keyLen, &orElem);

      bson_t regexDoc;
      bson_append_document_begin(&orElem, scopeKey, -1, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, regexBuf, "");
      bson_append_document_end(&orElem, &regexDoc);

      bson_append_document_end(&orArray, &orElem);
    }

    bson_append_array_end(filterP, &orArray);
    return;
  }

  //
  // Complex case: OR of AND groups
  //
  bson_t orArray;
  bson_append_array_begin(filterP, "$or", 3, &orArray);

  for (int gix = 0; gix < scopeExpr->groupCount; gix++)
  {
    char key[16];
    int  keyLen = snprintf(key, sizeof(key), "%d", gix);

    LdScopeGroup* grp = &scopeExpr->groupV[gix];
    bson_t        orElem;
    bson_append_document_begin(&orArray, key, keyLen, &orElem);

    if (grp->count == 1)
    {
      ldScopeToRegex(grp->scopeV[0], regexBuf, sizeof(regexBuf));

      bson_t regexDoc;
      bson_append_document_begin(&orElem, scopeKey, -1, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, regexBuf, "");
      bson_append_document_end(&orElem, &regexDoc);
    }
    else
    {
      //
      // One $and element per pattern of the group.
      //
      // An entity carrying a single scope stores it as a string, not as an array of one, and
      // $elemMatch never matches a string - so $all + $elemMatch would drop exactly those
      // entities. A plain $regex on the field matches a string as well as any element of an
      // array, which is also what the in-broker matcher does for the very same expression.
      //
      bson_t andArray;

      bson_append_array_begin(&orElem, "$and", 4, &andArray);

      for (int six = 0; six < grp->count; six++)
      {
        char skey[16];
        int  skeyLen = snprintf(skey, sizeof(skey), "%d", six);

        ldScopeToRegex(grp->scopeV[six], regexBuf, sizeof(regexBuf));

        bson_t andElem;
        bson_t regexDoc;

        bson_append_document_begin(&andArray, skey, skeyLen, &andElem);
        bson_append_document_begin(&andElem, scopeKey, -1, &regexDoc);
        bson_append_regex(&regexDoc, "$regex", 6, regexBuf, "");
        bson_append_document_end(&andElem, &regexDoc);
        bson_append_document_end(&andArray, &andElem);
      }

      bson_append_array_end(&orElem, &andArray);
    }

    bson_append_document_end(&orArray, &orElem);
  }

  bson_append_array_end(filterP, &orArray);
}



// -----------------------------------------------------------------------------
//
// jsonStrEscape - escape a string for embedding inside a JSON string literal
//
static void jsonStrEscape(char* dst, int dstSize, const char* src)
{
  int j = 0;
  for (int i = 0; (src[i] != 0) && (j < dstSize - 7); i++)
  {
    unsigned char c = (unsigned char) src[i];
    if      (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c;   }
    else if (c == '\n')             { dst[j++] = '\\'; dst[j++] = 'n'; }
    else if (c == '\r')             { dst[j++] = '\\'; dst[j++] = 'r'; }
    else if (c == '\t')             { dst[j++] = '\\'; dst[j++] = 't'; }
    else if (c < 0x20)              { j += snprintf(dst + j, dstSize - j, "\\u%04x", c); }
    else                             dst[j++] = c;
  }
  dst[j] = 0;
}



// -----------------------------------------------------------------------------
//
// bsonAppendLangWildcard - § 7.2.3.4 item 5 "[*]" (no natural language) match.
//
// MongoDB cannot wildcard the keys of an object in a plain field path, so the
// languageMap (an object at langPath) is turned into an array of {k,v} pairs
// with $objectToArray inside a $expr, and matched against ANY key's value —
// scalar or a single array element. Negative operators (!= / notPattern) require
// that NO key match, so the positive predicate is wrapped in $not. Relational
// operators are not meaningful over language text and are left to fall through
// (they resolve to a literal "*" key that never exists → no match), so this only
// handles == / != / pattern / notPattern.
//
static void bsonAppendLangWildcard(bson_t* docP, const char* langPath, LdQTerm* term)
{
  char pred[4096];   // per-element positive predicate over "$$kv.v"
  bool pattern = (term->op == LdQPattern) || (term->op == LdQNotPattern);

  if ((term->valueType == LdQString) || (term->valueType == LdQNoValue))
  {
    char esc[1024];
    jsonStrEscape(esc, sizeof(esc), term->value.s);
    if (pattern)
      snprintf(pred, sizeof(pred),
        "{\"$or\":["
          "{\"$and\":[{\"$eq\":[{\"$type\":\"$$kv.v\"},\"string\"]},{\"$regexMatch\":{\"input\":\"$$kv.v\",\"regex\":\"%s\"}}]},"
          "{\"$and\":[{\"$isArray\":\"$$kv.v\"},{\"$anyElementTrue\":{\"$map\":{\"input\":\"$$kv.v\",\"as\":\"e\",\"in\":{\"$and\":[{\"$eq\":[{\"$type\":\"$$e\"},\"string\"]},{\"$regexMatch\":{\"input\":\"$$e\",\"regex\":\"%s\"}}]}}}}]}"
        "]}", esc, esc);
    else
      snprintf(pred, sizeof(pred),
        "{\"$or\":[{\"$eq\":[\"$$kv.v\",\"%s\"]},{\"$and\":[{\"$isArray\":\"$$kv.v\"},{\"$in\":[\"%s\",\"$$kv.v\"]}]}]}", esc, esc);
  }
  else if (term->valueType == LdQNumber)
  {
    char num[64];
    snprintf(num, sizeof(num), "%.17g", term->value.n);
    snprintf(pred, sizeof(pred),
      "{\"$or\":[{\"$eq\":[\"$$kv.v\",%s]},{\"$and\":[{\"$isArray\":\"$$kv.v\"},{\"$in\":[%s,\"$$kv.v\"]}]}]}", num, num);
  }
  else if (term->valueType == LdQBool)
  {
    const char* b = term->value.b ? "true" : "false";
    snprintf(pred, sizeof(pred),
      "{\"$or\":[{\"$eq\":[\"$$kv.v\",%s]},{\"$and\":[{\"$isArray\":\"$$kv.v\"},{\"$in\":[%s,\"$$kv.v\"]}]}]}", b, b);
  }
  else
    return;   // unsupported value type for a "[*]" term

  bool negative = (term->op == LdQUnequal) || (term->op == LdQNotPattern);

  char any[8192];
  snprintf(any, sizeof(any),
    "{\"$anyElementTrue\":{\"$map\":{\"input\":{\"$objectToArray\":\"$%s\"},\"as\":\"kv\",\"in\":%s}}}",
    langPath, pred);

  char cond[8320];
  if (negative)
    snprintf(cond, sizeof(cond), "{\"$not\":[%s]}", any);
  else
    snprintf(cond, sizeof(cond), "%s", any);

  bson_error_t error;
  bson_t*      condP = bson_new_from_json((const uint8_t*) cond, -1, &error);
  if (condP != NULL)
  {
    bson_append_document(docP, "$expr", 5, condP);
    bson_destroy(condP);
  }
}



// -----------------------------------------------------------------------------
//
// bsonAppendMultiInstanceTerm - one q term against EVERY instance of an Attribute
//
// Clause 7: with several instances and no datasetId addressed, the target value
// is "any Value of such instances". The field path we would otherwise build is
// <attr>.@none.value, which only ever reaches the DEFAULT instance - so a value
// living on a datasetId instance was invisible to q, and an Attribute with no
// default instance at all could not be matched by any comparison term.
//
// Instance keys are datasetIds, i.e. arbitrary and unknown at query-build time,
// and Mongo cannot wildcard object keys in a field path. That is the same
// obstacle the "[*]" languageMap term already has, so the same answer:
// $objectToArray over the attribute, then $anyElementTrue over a per-instance
// predicate. Sub-attribute and value-path segments just extend that predicate.
//
// Positive operators take ANY instance; the negative ones need EVERY instance to
// satisfy them, which is "no instance satisfies the positive form" - exactly how
// bsonAppendLangWildcard negates, and what the in-broker matcher does.
//
// Returns false when the term shape is not handled here, and the caller falls
// back to the plain field path.
//
static bool bsonAppendMultiInstanceTerm(bson_t* docP, const char* attrPath, LdQTerm* term)
{
  //
  // Where the value sits INSIDE one instance: $$kv.v[.sub...].value[.member...]
  // (the DB model renames every value key to "value" whatever the Attribute
  // type, so this one spelling covers a Relationship's object too).
  //
  char vexpr[1024];
  int  vp = snprintf(vexpr, sizeof(vexpr), "$$kv.v");

  for (int i = 0; i < term->subPathN; i++)
    vp += snprintf(vexpr + vp, sizeof(vexpr) - vp, ".%s", mongocEscapeDotsInKey(term->subPathV[i]));

  vp += snprintf(vexpr + vp, sizeof(vexpr) - vp, ".value");

  for (int i = 0; i < term->valuePathN; i++)
  {
    if (strcmp(term->valuePathV[i], "*") == 0)
      return false;                                  // "[*]" keeps its own handling
    vp += snprintf(vexpr + vp, sizeof(vexpr) - vp, ".%s", mongocEscapeDotsInKey(term->valuePathV[i]));
  }

  //
  // The per-instance POSITIVE predicate. Negative operators reuse it and negate
  // the whole $anyElementTrue, so this only ever builds the positive form.
  //
  char        pred[8192];
  bool        pattern = (term->op == LdQPattern) || (term->op == LdQNotPattern);
  const char* relOp   = NULL;

  switch (term->op)
  {
  case LdQGreater:   relOp = "$gt";  break;
  case LdQLess:      relOp = "$lt";  break;
  case LdQGreaterEq: relOp = "$gte"; break;
  case LdQLessEq:    relOp = "$lte"; break;
  default:                           break;
  }

  if (term->valueType == LdQString)
  {
    if (term->value.s == NULL)
      return false;

    char esc[1024];
    jsonStrEscape(esc, sizeof(esc), term->value.s);

    if (pattern)
      snprintf(pred, sizeof(pred),
        "{\"$and\":[{\"$eq\":[{\"$type\":\"%s\"},\"string\"]},{\"$regexMatch\":{\"input\":\"%s\",\"regex\":\"%s\"}}]}",
        vexpr, vexpr, esc);
    else if (relOp != NULL)
      // Guard the type: Mongo orders ACROSS BSON types, so an unguarded $gt
      // would let a number satisfy a string comparison.
      snprintf(pred, sizeof(pred),
        "{\"$and\":[{\"$eq\":[{\"$type\":\"%s\"},\"string\"]},{\"%s\":[\"%s\",\"%s\"]}]}",
        vexpr, relOp, vexpr, esc);
    else
      snprintf(pred, sizeof(pred),
        "{\"$or\":[{\"$eq\":[\"%s\",\"%s\"]},{\"$and\":[{\"$isArray\":\"%s\"},{\"$in\":[\"%s\",\"%s\"]}]}]}",
        vexpr, esc, vexpr, esc, vexpr);
  }
  else if (term->valueType == LdQNumber)
  {
    if (pattern)
      return false;

    char num[64];
    snprintf(num, sizeof(num), "%.17g", term->value.n);

    if (relOp != NULL)
      snprintf(pred, sizeof(pred),
        "{\"$and\":[{\"$in\":[{\"$type\":\"%s\"},[\"double\",\"int\",\"long\",\"decimal\"]]},{\"%s\":[\"%s\",%s]}]}",
        vexpr, relOp, vexpr, num);
    else
      snprintf(pred, sizeof(pred),
        "{\"$or\":[{\"$eq\":[\"%s\",%s]},{\"$and\":[{\"$isArray\":\"%s\"},{\"$in\":[%s,\"%s\"]}]}]}",
        vexpr, num, vexpr, num, vexpr);
  }
  else if (term->valueType == LdQBool)
  {
    if (pattern || (relOp != NULL))
      return false;

    const char* b = term->value.b ? "true" : "false";
    snprintf(pred, sizeof(pred),
      "{\"$or\":[{\"$eq\":[\"%s\",%s]},{\"$and\":[{\"$isArray\":\"%s\"},{\"$in\":[%s,\"%s\"]}]}]}",
      vexpr, b, vexpr, b, vexpr);
  }
  else
    return false;                                    // ranges / value-lists keep the old path

  bool negative = (term->op == LdQUnequal) || (term->op == LdQNotPattern);

  char any[16384];
  snprintf(any, sizeof(any),
    "{\"$and\":["
      "{\"$eq\":[{\"$type\":\"$%s\"},\"object\"]},"
      "{\"$anyElementTrue\":{\"$map\":{\"input\":{\"$objectToArray\":\"$%s\"},\"as\":\"kv\",\"in\":%s}}}"
    "]}",
    attrPath, attrPath, pred);

  char cond[24576];   // must hold `any` (16K) plus the negation wrapper
  if (negative)
  {
    //
    // EVERY instance must satisfy != / notPattern, i.e. none satisfies the
    // positive form - but the value under test must still be PRESENT, since an
    // Entity that does not have it does not "have a value different from X"
    // (§ 4.9: a term over an attrPath that is not in the Entity is false).
    //
    // Two guards, and both are needed. The $type check on attrPath says the
    // Attribute is there; it says nothing about the rest of the path, so on
    // `q=a.b.c!=1` an Entity holding `a` and no `a.b.c` used to pass it and
    // then match, because $not over a missing field is true. That answer also
    // contradicted the same backend's reply to `q=!a.b.c`, which correctly
    // listed the very same Entity as NOT having the path.
    //
    // The second guard requires the full per-instance expression - sub-path,
    // value and value-path included - to resolve in at least one instance. At
    // depth 0 it costs nothing: a stored Attribute always has its value.
    //
    char presence[4096];
    snprintf(presence, sizeof(presence),
      "{\"$anyElementTrue\":{\"$map\":{\"input\":{\"$objectToArray\":\"$%s\"},\"as\":\"kv\","
      "\"in\":{\"$ne\":[{\"$type\":\"%s\"},\"missing\"]}}}}",
      attrPath, vexpr);

    snprintf(cond, sizeof(cond),
      "{\"$and\":[{\"$eq\":[{\"$type\":\"$%s\"},\"object\"]},%s,{\"$not\":[%s]}]}",
      attrPath, presence, any);
  }
  else
    snprintf(cond, sizeof(cond), "%s", any);

  bson_error_t error;
  bson_t*      condP = bson_new_from_json((const uint8_t*) cond, -1, &error);
  if (condP == NULL)
    return false;

  bson_append_document(docP, "$expr", 5, condP);
  bson_destroy(condP);

  return true;
}



// -----------------------------------------------------------------------------
//
// bsonAppendQTerm - build BSON filter for a single Q term
//
static void bsonAppendQTerm(bson_t* docP, LdQTerm* term)
{
  //
  // Field path. Mongo doc shape:
  //   attr.@none.value                      plain term
  //   attr.@none.sub1[...].value            § 4.9 attrPath — sub-attrs are
  //                                         flat objects inside the instance
  // mongocEscapeDotsInKey returns a shared thread-local buffer — copy each
  // segment into `path` before the next call.
  //
  //
  // The three timestamps — createdAt / modifiedAt / observedAt — are special:
  // they are stored as int64 (nanosecond) fields, NOT as {value:...} objects.
  // createdAt/modifiedAt sit at the entity top level; all three appear as the
  // int64 leaf of an attribute instance (e.g. <attr>.@none.observedAt). q over
  // them is a NUMERIC compare against term->value.ns.
  //
  // LdQValue is a union: for an LdQDateTime term the `ns` slot is live, so the
  // overlapping `.s` slot holds those bytes reinterpreted as a pointer — it must
  // NEVER be read as a string (bson_append_utf8 would strlen a wild address).
  //
  {
    const char* leaf = (term->subPathN > 0) ? term->subPathV[term->subPathN - 1] : term->attr;
    bool        isTimestamp = (term->valueType == LdQDateTime) && (term->valuePathN == 0) &&
                              ((strcmp(leaf, "observedAt") == 0) ||
                               (strcmp(leaf, "createdAt")  == 0) ||
                               (strcmp(leaf, "modifiedAt") == 0));
    // observedAt is not an entity-level field — only an attribute-instance one.
    if (isTimestamp && (term->subPathN == 0) && (strcmp(term->attr, "observedAt") == 0))
      isTimestamp = false;

    if (isTimestamp)
    {
      char tsPath[1024];
      int  tp = snprintf(tsPath, sizeof(tsPath), "%s", mongocEscapeDotsInKey(term->attr));
      if (term->subPathN > 0)
      {
        tp += snprintf(tsPath + tp, sizeof(tsPath) - tp, ".@none");
        for (int i = 0; i < term->subPathN; i++)
          tp += snprintf(tsPath + tp, sizeof(tsPath) - tp, ".%s", mongocEscapeDotsInKey(term->subPathV[i]));
      }

      if (term->op == LdQEqual)
      {
        bson_append_int64(docP, tsPath, -1, term->value.ns);
      }
      else
      {
        const char* mongoOp = NULL;
        switch (term->op)
        {
        case LdQUnequal:   mongoOp = "$ne";  break;
        case LdQGreater:   mongoOp = "$gt";  break;
        case LdQLess:      mongoOp = "$lt";  break;
        case LdQGreaterEq: mongoOp = "$gte"; break;
        case LdQLessEq:    mongoOp = "$lte"; break;
        default:           mongoOp = "$eq";  break;
        }
        bson_t opDoc;
        bson_append_document_begin(docP, tsPath, -1, &opDoc);
        bson_append_int64(&opDoc, mongoOp, -1, term->value.ns);
        if (term->op == LdQUnequal)
        bson_append_bool(&opDoc, "$exists", 7, true);
        bson_append_document_end(docP, &opDoc);
      }
      return;
    }
  }

  char path[1024];
  int  pos = snprintf(path, sizeof(path), "%s", mongocEscapeDotsInKey(term->attr));
  if (term->subPathN > 0)
  {
    pos += snprintf(path + pos, sizeof(path) - pos, ".@none");
    for (int i = 0; i < term->subPathN; i++)
      pos += snprintf(path + pos, sizeof(path) - pos, ".%s", mongocEscapeDotsInKey(term->subPathV[i]));
  }

  //
  // Existence check — the path so far names the attribute (or final
  // sub-attribute) object itself; with a "[...]" value path, existence
  // is of the FINAL value member.
  //
  if ((term->op == LdQExists || term->op == LdQNotExists) && term->valuePathN == 0)
  {
    bson_t existsDoc;
    bson_append_document_begin(docP, path, -1, &existsDoc);
    bson_append_bool(&existsDoc, "$exists", 7, (term->op == LdQExists));
    bson_append_document_end(docP, &existsDoc);
    return;
  }

  //
  // Value leaf. Before falling back to the fixed <attr>.@none.value path — which
  // sees the default instance and nothing else — try the any-instance form
  // (§ 8.5 + clause 7). It handles the ordinary comparison operators; anything
  // it declines (ranges, value-lists, "[*]") drops through unchanged.
  //
  {
    char attrPath[1024];
    snprintf(attrPath, sizeof(attrPath), "%s", mongocEscapeDotsInKey(term->attr));

    if (bsonAppendMultiInstanceTerm(docP, attrPath, term) == true)
      return;
  }

  pos += snprintf(path + pos, sizeof(path) - pos, "%s.value", (term->subPathN > 0) ? "" : ".@none");

  //
  // § 7.2.3.4 item 5 — "[*]" (no natural language specified): match ANY key of
  // the LanguageProperty's languageMap. MongoDB can't wildcard object keys in a
  // field path, so this compiles to a $expr over $objectToArray (path so far
  // names the languageMap object).
  //
  if ((term->valuePathN == 1) && (strcmp(term->valuePathV[0], "*") == 0))
  {
    if ((term->op == LdQExists) || (term->op == LdQNotExists))
    {
      bson_t existsDoc;
      bson_append_document_begin(docP, path, -1, &existsDoc);
      bson_append_bool(&existsDoc, "$exists", 7, (term->op == LdQExists));
      bson_append_document_end(docP, &existsDoc);
      return;
    }
    if ((term->op == LdQEqual) || (term->op == LdQUnequal) ||
        (term->op == LdQPattern) || (term->op == LdQNotPattern))
    {
      bsonAppendLangWildcard(docP, path, term);
      return;
    }
    // relational operators over "[*]" fall through (they never match — see helper)
  }

  //
  // § 4.9 "[...]" — descend INTO the value: opaque member names.
  //
  for (int i = 0; i < term->valuePathN; i++)
    pos += snprintf(path + pos, sizeof(path) - pos, ".%s", mongocEscapeDotsInKey(term->valuePathV[i]));

  if (term->op == LdQExists || term->op == LdQNotExists)
  {
    bson_t existsDoc;
    bson_append_document_begin(docP, path, -1, &existsDoc);
    bson_append_bool(&existsDoc, "$exists", 7, (term->op == LdQExists));
    bson_append_document_end(docP, &existsDoc);
    return;
  }

  //
  // Simple equality / inequality with single value
  //
  if (term->valueType == LdQNumber)
  {
    if (term->op == LdQEqual)
    {
      bson_append_double(docP, path, -1, term->value.n);
    }
    else
    {
      const char* mongoOp = NULL;

      switch (term->op)
      {
      case LdQUnequal:   mongoOp = "$ne";  break;
      case LdQGreater:   mongoOp = "$gt";  break;
      case LdQLess:      mongoOp = "$lt";  break;
      case LdQGreaterEq: mongoOp = "$gte"; break;
      case LdQLessEq:    mongoOp = "$lte"; break;
      default:           mongoOp = "$eq";  break;
      }

      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_double(&opDoc, mongoOp, -1, term->value.n);
      if (term->op == LdQUnequal)
        bson_append_bool(&opDoc, "$exists", 7, true);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if (term->valueType == LdQString)
  {
    if (term->op == LdQEqual)
    {
      bson_append_utf8(docP, path, -1, term->value.s, -1);
    }
    else if (term->op == LdQUnequal)
    {
      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_utf8(&opDoc, "$ne", 3, term->value.s, -1);
      bson_append_bool(&opDoc, "$exists", 7, true);
      bson_append_document_end(docP, &opDoc);
    }
    else if (term->op == LdQPattern)
    {
      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_regex(&opDoc, "$regex", 6, term->value.s, "");
      bson_append_document_end(docP, &opDoc);
    }
    else if (term->op == LdQNotPattern)
    {
      bson_t notDoc;
      bson_t regexDoc;
      bson_append_document_begin(docP, path, -1, &notDoc);
      bson_append_document_begin(&notDoc, "$not", 4, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, term->value.s, "");
      bson_append_document_end(&notDoc, &regexDoc);
      bson_append_bool(&notDoc, "$exists", 7, true);
      bson_append_document_end(docP, &notDoc);
    }
    else
    {
      const char* mongoOp = NULL;

      switch (term->op)
      {
      case LdQGreater:   mongoOp = "$gt";  break;
      case LdQLess:      mongoOp = "$lt";  break;
      case LdQGreaterEq: mongoOp = "$gte"; break;
      case LdQLessEq:    mongoOp = "$lte"; break;
      default:           mongoOp = "$eq";  break;
      }

      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_utf8(&opDoc, mongoOp, -1, term->value.s, -1);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if (term->valueType == LdQBool)
  {
    if (term->op == LdQEqual)
    {
      bson_append_bool(docP, path, -1, term->value.b);
    }
    else if (term->op == LdQUnequal)
    {
      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_bool(&opDoc, "$ne", 3, term->value.b);
      bson_append_bool(&opDoc, "$exists", 7, true);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if (term->valueType == LdQDateTime)
  {
    //
    // A DateTime compared against a regular attribute value (the createdAt /
    // modifiedAt / observedAt timestamps are int64 and handled at the top of
    // this function). The value lives in the union's ns slot — compare as int64,
    // NEVER as a string (term->value.s aliases the ns bytes as a wild pointer).
    //
    if (term->op == LdQEqual)
    {
      bson_append_int64(docP, path, -1, term->value.ns);
    }
    else
    {
      const char* mongoOp = NULL;

      switch (term->op)
      {
      case LdQUnequal:   mongoOp = "$ne";  break;
      case LdQGreater:   mongoOp = "$gt";  break;
      case LdQLess:      mongoOp = "$lt";  break;
      case LdQGreaterEq: mongoOp = "$gte"; break;
      case LdQLessEq:    mongoOp = "$lte"; break;
      default:           mongoOp = "$eq";  break;
      }

      bson_t opDoc;
      bson_append_document_begin(docP, path, -1, &opDoc);
      bson_append_int64(&opDoc, mongoOp, -1, term->value.ns);
      if (term->op == LdQUnequal)
        bson_append_bool(&opDoc, "$exists", 7, true);
      bson_append_document_end(docP, &opDoc);
    }
  }
  else if ((term->valueType == LdQRange) || (term->valueType == LdQDateRange))
  {
    //
    // § 4.9: a range is only meaningful for == and !=, and the negation has to
    // be built in — otherwise "p!=1..50" is assembled as the very same
    // $gte/$lte as "p==1..50" and selects exactly the Entities it should
    // exclude. Mongo's field-level $not takes an operator document, so the
    // bounds are wrapped rather than re-derived.
    //
    bool    negated = (term->op == LdQUnequal);
    bson_t  notDoc;
    bson_t  rangeDoc;

    if (negated)
      bson_append_document_begin(docP, path, -1, &notDoc);

    bson_append_document_begin(negated ? &notDoc : docP, negated ? "$not" : path,
                               negated ? 4 : -1, &rangeDoc);

    if (term->valueType == LdQRange)
    {
      bson_append_double(&rangeDoc, "$gte", 4, term->value.numRange.lo);
      bson_append_double(&rangeDoc, "$lte", 4, term->value.numRange.hi);
    }
    else
    {
      bson_append_utf8(&rangeDoc, "$gte", 4, term->value.dateRange.lo, -1);
      bson_append_utf8(&rangeDoc, "$lte", 4, term->value.dateRange.hi, -1);
    }

    bson_append_document_end(negated ? &notDoc : docP, &rangeDoc);

    if (negated)
    {
      bson_append_bool(&notDoc, "$exists", 7, true);
      bson_append_document_end(docP, &notDoc);
    }
  }
  else if (term->valueType == LdQValueList)
  {
    const char* mongoOp = (term->op == LdQEqual) ? "$in" : "$nin";
    int         mongoOpLen = (term->op == LdQEqual) ? 3 : 4;

    bson_t inDoc;
    bson_t array;
    bson_append_document_begin(docP, path, -1, &inDoc);
    bson_append_array_begin(&inDoc, mongoOp, mongoOpLen, &array);

    for (int i = 0; i < term->value.list.count; i++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", i);

      if (term->value.list.itemType == LdQNumber)
        bson_append_double(&array, key, keyLen, strtod(term->value.list.values[i], NULL));
      else if (term->value.list.itemType == LdQBool)
        bson_append_bool(&array, key, keyLen, strcmp(term->value.list.values[i], "true") == 0);
      else
        bson_append_utf8(&array, key, keyLen, term->value.list.values[i], -1);
    }

    bson_append_array_end(&inDoc, &array);

    if (term->op != LdQEqual)
      bson_append_bool(&inDoc, "$exists", 7, true);
    bson_append_document_end(docP, &inDoc);
  }
}



// -----------------------------------------------------------------------------
//
// bsonAppendQNode - recursively build BSON filter from Q expression tree
//
static void bsonAppendQNode(bson_t* docP, LdQNode* nodeP)
{
  if (nodeP->type == LdQTermNode)
  {
    bsonAppendQTerm(docP, &nodeP->term);
  }
  else if (nodeP->type == LdQAndNode)
  {
    bson_t andArray;
    bson_append_array_begin(docP, "$and", 4, &andArray);

    for (int i = 0; i < nodeP->group.count; i++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", i);

      bson_t childDoc;
      bson_append_document_begin(&andArray, key, keyLen, &childDoc);
      bsonAppendQNode(&childDoc, nodeP->group.childV[i]);
      bson_append_document_end(&andArray, &childDoc);
    }

    bson_append_array_end(docP, &andArray);
  }
  else if (nodeP->type == LdQOrNode)
  {
    bson_t orArray;
    bson_append_array_begin(docP, "$or", 3, &orArray);

    for (int i = 0; i < nodeP->group.count; i++)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", i);

      bson_t childDoc;
      bson_append_document_begin(&orArray, key, keyLen, &childDoc);
      bsonAppendQNode(&childDoc, nodeP->group.childV[i]);
      bson_append_document_end(&orArray, &childDoc);
    }

    bson_append_array_end(docP, &orArray);
  }
}



// -----------------------------------------------------------------------------
//
// bsonAppendQFilter - add Q expression filter to a BSON document
//
static void bsonAppendQFilter(bson_t* filterP, LdQNode* qExpr)
{
  bsonAppendQNode(filterP, qExpr);
}



// -----------------------------------------------------------------------------
//
// bsonAppendCoordinates - recursively append a JSON coordinate array to BSON
//
static const char* bsonAppendCoordValue(bson_t* arrayP, int* indexP, const char* p);

static const char* bsonAppendCoordArray(bson_t* parentP, const char* key, int keyLen, const char* p)
{
  bson_t array;
  bson_append_array_begin(parentP, key, keyLen, &array);

  int index = 0;

  // Skip '['
  p++;

  while (*p != 0 && *p != ']')
  {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
      p++;

    if (*p == ']')
      break;

    p = bsonAppendCoordValue(&array, &index, p);

    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
      p++;

    if (*p == ',')
      p++;
  }

  if (*p == ']')
    p++;

  bson_append_array_end(parentP, &array);
  return p;
}

static const char* bsonAppendCoordValue(bson_t* arrayP, int* indexP, const char* p)
{
  char key[16];
  int  keyLen = snprintf(key, sizeof(key), "%d", *indexP);
  (*indexP)++;

  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
    p++;

  if (*p == '[')
  {
    // Nested array
    return bsonAppendCoordArray(arrayP, key, keyLen, p);
  }
  else
  {
    // Number
    char* end;
    double val = strtod(p, &end);
    bson_append_double(arrayP, key, keyLen, val);
    return end;
  }
}



// -----------------------------------------------------------------------------
//
// bsonAppendGeoFilter - build BSON geo-query filter from DbQueryFilter geo fields
//
static void bsonAppendGeoFilter(bson_t* filterP, DbQueryFilter* f)
{
  if (f->geoRel == NULL || f->geometry == NULL || f->coordinates == NULL)
    return;

  char fieldPath[1024];
  snprintf(fieldPath, sizeof(fieldPath), "%s.@none.value", mongocEscapeDotsInKey(f->geoproperty));

  // Build reference geometry BSON
  bson_t geometry;
  bson_init(&geometry);
  bson_append_utf8(&geometry, "type", 4, f->geometry, -1);

  // Parse coordinates JSON string into BSON array
  const char* coordStr = f->coordinates;
  while (*coordStr == ' ') coordStr++;

  if (*coordStr == '[')
    bsonAppendCoordArray(&geometry, "coordinates", 11, coordStr);

  switch (f->geoRel->rel)
  {
  case LdGeoNear:
  {
    bson_t fieldDoc;
    bson_t nearDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$near", 5, &nearDoc);
    bson_append_document(&nearDoc, "$geometry", 9, &geometry);
    if (f->geoRel->maxDistance >= 0)
      bson_append_double(&nearDoc, "$maxDistance", 12, f->geoRel->maxDistance);
    if (f->geoRel->minDistance >= 0)
      bson_append_double(&nearDoc, "$minDistance", 12, f->geoRel->minDistance);
    bson_append_document_end(&fieldDoc, &nearDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoWithin:
  {
    bson_t fieldDoc;
    bson_t withinDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$geoWithin", 10, &withinDoc);
    bson_append_document(&withinDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&fieldDoc, &withinDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoIntersects:
  {
    bson_t fieldDoc;
    bson_t intersectsDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$geoIntersects", 14, &intersectsDoc);
    bson_append_document(&intersectsDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&fieldDoc, &intersectsDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoOverlaps:
  {
    //
    // § 7.2.4 overlap is the OGC 06-103r4 one: same dimension, interiors
    // meeting, neither geometry containing the other. Mongo has no
    // $geoOverlaps, so this is the closest pushdown: intersecting but not
    // within the reference, which drops the "entity within reference" and
    // "entity equals reference" cases.
    //
    // APPROXIMATION: an entity whose geometry CONTAINS the reference still
    // matches here (Mongo cannot express containment in the other direction,
    // see LdGeoContains below), and the same-dimension rule is not applied.
    // The in-broker matcher (plugins/shared/geoMatch.c), which serves the
    // corDB backend, subscriptions, snapshots and the temporal API, is
    // exact — so the two backends deliberately differ for those cases.
    //
    // The negation goes in a top-level $nor rather than next to the
    // $geoIntersects: Mongo lets no sibling share an operator document with a
    // geo operator ("can't parse extra field"). $nor alone would also match
    // Entities without the GeoProperty, but the $geoIntersects clause already
    // requires it, keeping § 7.2.4's "no target GeoProperty ⇒ non-matching".
    //
    bson_t fieldDoc;
    bson_t intersectsDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$geoIntersects", 14, &intersectsDoc);
    bson_append_document(&intersectsDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&fieldDoc, &intersectsDoc);
    bson_append_document_end(filterP, &fieldDoc);

    bson_t norArray;
    bson_t norElem;
    bson_t norField;
    bson_t withinDoc;
    bson_append_array_begin(filterP, "$nor", 4, &norArray);
    bson_append_document_begin(&norArray, "0", 1, &norElem);
    bson_append_document_begin(&norElem, fieldPath, -1, &norField);
    bson_append_document_begin(&norField, "$geoWithin", 10, &withinDoc);
    bson_append_document(&withinDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&norField, &withinDoc);
    bson_append_document_end(&norElem, &norField);
    bson_append_document_end(&norArray, &norElem);
    bson_append_array_end(filterP, &norArray);
    break;
  }

  case LdGeoDisjoint:
  {
    //
    // $exists is what keeps § 7.2.4's closing rule — "Entities which do not
    // convey the target GeoProperty of the query shall be considered as
    // non-matching" — true here. Mongo's $not matches a MISSING field, so
    // without it every geoproperty-less Entity would come back as disjoint.
    //
    bson_t fieldDoc;
    bson_t notDoc;
    bson_t intersectsDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$not", 4, &notDoc);
    bson_append_document_begin(&notDoc, "$geoIntersects", 14, &intersectsDoc);
    bson_append_document(&intersectsDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&notDoc, &intersectsDoc);
    bson_append_document_end(&fieldDoc, &notDoc);
    bson_append_bool(&fieldDoc, "$exists", 7, true);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoEquals:
  {
    //
    // APPROXIMATION: an exact match on the stored GeoJSON sub-document, not
    // the geometric equality § 7.2.4 asks for. The same square written from
    // another starting vertex, or with the opposite ring orientation, is the
    // same geometry but a different document, so it is missed here. Mongo has
    // no geometric-equality operator; geoMatch.c uses GEOSEquals and is exact.
    //
    bson_append_document(filterP, fieldPath, -1, &geometry);
    break;
  }

  case LdGeoContains:
  {
    //
    // APPROXIMATION: § 7.2.4 asks for the ENTITY's geometry to contain the
    // reference, and Mongo has no $geoContains — $geoWithin only tests the
    // other direction. $geoIntersects is a superset of the right answer: it
    // is exact when the reference is a Point (a polygon intersects a point
    // iff it contains it, boundaries aside) and over-matches for anything
    // larger. geoMatch.c uses GEOSContains and is exact.
    //
    bson_t fieldDoc;
    bson_t intersectsDoc;
    bson_append_document_begin(filterP, fieldPath, -1, &fieldDoc);
    bson_append_document_begin(&fieldDoc, "$geoIntersects", 14, &intersectsDoc);
    bson_append_document(&intersectsDoc, "$geometry", 9, &geometry);
    bson_append_document_end(&fieldDoc, &intersectsDoc);
    bson_append_document_end(filterP, &fieldDoc);
    break;
  }

  case LdGeoNone:
    break;
  }

  bson_destroy(&geometry);
}



// -----------------------------------------------------------------------------
//
// bsonAppendNonGeoMatch - append the non-geo filter clauses (id / idPattern /
// type / scope / q) to a $match document. Shared by the $geoNear pipeline's
// $match stage and the dist-sort $unionWith non-geo sub-pipeline.
//
static void bsonAppendNonGeoMatch(bson_t* matchFilter, DbQueryFilter* filterP)
{
  if (filterP->idV != NULL)
    bsonInArray(matchFilter, "_id", filterP->idV);
  if (filterP->idPattern != NULL)
  {
    bson_t regexDoc;
    bson_append_document_begin(matchFilter, "_id", 3, &regexDoc);
    bson_append_regex(&regexDoc, "$regex", 6, filterP->idPattern, "");
    bson_append_document_end(matchFilter, &regexDoc);
  }
  if (filterP->typeExpr != NULL && !filterP->typeExpr->isSimple)
  {
    bson_t orArray;
    bson_append_array_begin(matchFilter, "$or", 3, &orArray);
    for (int gix = 0; gix < filterP->typeExpr->groupCount; gix++)
    {
      char key2[16];
      int  keyLen2 = snprintf(key2, sizeof(key2), "%d", gix);
      LdTypeGroup* grp = &filterP->typeExpr->groupV[gix];
      bson_t orElem;
      bson_append_document_begin(&orArray, key2, keyLen2, &orElem);
      if (grp->count == 1)
        bson_append_utf8(&orElem, "type", 4, grp->typeV[0], -1);
      else
      {
        bson_t allDoc, allArray;
        bson_append_document_begin(&orElem, "type", 4, &allDoc);
        bson_append_array_begin(&allDoc, "$all", 4, &allArray);
        for (int tix = 0; tix < grp->count; tix++)
        {
          char tkey[16];
          int  tkeyLen = snprintf(tkey, sizeof(tkey), "%d", tix);
          bson_append_utf8(&allArray, tkey, tkeyLen, grp->typeV[tix], -1);
        }
        bson_append_array_end(&allDoc, &allArray);
        bson_append_document_end(&orElem, &allDoc);
      }
      bson_append_document_end(&orArray, &orElem);
    }
    bson_append_array_end(matchFilter, &orArray);
  }
  else if (filterP->typeV != NULL)
    bsonInArray(matchFilter, "type", filterP->typeV);
  if (filterP->scopeExpr != NULL)
    bsonAppendScopeFilter(matchFilter, filterP->scopeExpr);
  if (filterP->qExpr != NULL)
    bsonAppendQFilter(matchFilter, filterP->qExpr);
}



// -----------------------------------------------------------------------------
//
// mongocEntityQuery -
//
int mongocEntityQuery(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP)
{
  mongoc_client_t*      clientP = mongoc_client_pool_pop(poolP);
  mongoc_collection_t*  collP   = mongoc_client_get_collection(clientP, tenantP->dbName, "entities");

  //
  // Build BSON filter from DbQueryFilter
  //
  bson_t filter;
  bson_init(&filter);

  if (filterP != NULL)
  {
    if (filterP->idV != NULL)
      bsonInArray(&filter, "_id", filterP->idV);

    if (filterP->idPattern != NULL)
    {
      bson_t regexDoc;
      bson_append_document_begin(&filter, "_id", 3, &regexDoc);
      bson_append_regex(&regexDoc, "$regex", 6, filterP->idPattern, "");
      bson_append_document_end(&filter, &regexDoc);
    }

    if (filterP->typeExpr != NULL && !filterP->typeExpr->isSimple)
    {
      bson_t orArray;
      bson_append_array_begin(&filter, "$or", 3, &orArray);

      for (int gix = 0; gix < filterP->typeExpr->groupCount; gix++)
      {
        char key[16];
        int  keyLen = snprintf(key, sizeof(key), "%d", gix);

        LdTypeGroup* grp = &filterP->typeExpr->groupV[gix];
        bson_t       orElem;
        bson_append_document_begin(&orArray, key, keyLen, &orElem);

        if (grp->count == 1)
        {
          bson_append_utf8(&orElem, "type", 4, grp->typeV[0], -1);
        }
        else
        {
          bson_t allDoc;
          bson_t allArray;

          bson_append_document_begin(&orElem, "type", 4, &allDoc);
          bson_append_array_begin(&allDoc, "$all", 4, &allArray);

          for (int tix = 0; tix < grp->count; tix++)
          {
            char tkey[16];
            int  tkeyLen = snprintf(tkey, sizeof(tkey), "%d", tix);
            bson_append_utf8(&allArray, tkey, tkeyLen, grp->typeV[tix], -1);
          }

          bson_append_array_end(&allDoc, &allArray);
          bson_append_document_end(&orElem, &allDoc);
        }

        bson_append_document_end(&orArray, &orElem);
      }

      bson_append_array_end(&filter, &orArray);
    }
    else if (filterP->typeV != NULL)
      bsonInArray(&filter, "type", filterP->typeV);

    if (filterP->scopeExpr != NULL)
      bsonAppendScopeFilter(&filter, filterP->scopeExpr);

    if (filterP->qExpr != NULL)
      bsonAppendQFilter(&filter, filterP->qExpr);

    // For georel=near, the $geoNear aggregation stage handles the geo filter.
    // For other geo relations, add the filter to the find() query.
    if (filterP->geoRel != NULL && filterP->geoRel->rel != LdGeoNear)
      bsonAppendGeoFilter(&filter, filterP);
  }

  //
  // Build query options (limit + skip)
  //
  bson_t opts;
  bson_init(&opts);

  //
  // limit=0 means "count only, no results"
  //
  bool countOnly = (filterP != NULL && filterP->limit == 0 && filterP->count);

  if (filterP != NULL && filterP->limit > 0)
    BSON_APPEND_INT64(&opts, "limit", filterP->limit);

  if (filterP != NULL && filterP->offset > 0)
    BSON_APPEND_INT64(&opts, "skip", filterP->offset);

  //
  // Sort by createdAt (ascending), with _id as a unique tiebreak. createdAt
  // alone is NOT unique — entities created in one batch share it — and Mongo's
  // sort is unstable for equal keys, so paginating (skip/limit) across separate
  // queries would overlap and skip rows. _id (the entity id) is unique, making
  // the order total and pagination deterministic.
  //
  bson_t sort;
  bson_init(&sort);
  BSON_APPEND_INT32(&sort, "createdAt", 1);
  BSON_APPEND_INT32(&sort, "_id", 1);
  BSON_APPEND_DOCUMENT(&opts, "sort", &sort);
  bson_destroy(&sort);

  //
  // Count total matching documents (before limit/offset) if requested.
  //
  // For georel=near, `$near` inside a regular filter cannot survive
  // mongoc_collection_count_documents (Mongo silently ignores it in
  // the synthetic aggregate it builds), so the count would come back
  // as the unfiltered total. Build a dedicated $geoNear→$count
  // pipeline instead. All other geo predicates ($geoIntersects /
  // $geoWithin / etc.) work fine inside count_documents.
  //
  bool useGeoNearForCount = (filterP != NULL && filterP->geoRel != NULL && filterP->geoRel->rel == LdGeoNear);

  if (filterP != NULL && filterP->count)
  {
    if (useGeoNearForCount)
    {
      char fieldPath[1024];
      snprintf(fieldPath, sizeof(fieldPath), "%s.@none.value", mongocEscapeDotsInKey(filterP->geoproperty));

      bson_t pipeline;
      bson_init(&pipeline);

      bson_t stages;
      bson_append_array_begin(&pipeline, "pipeline", 8, &stages);
      int stageIx = 0;

      // $geoNear stage — mirrors the body pipeline below, sans
      // distanceField (we only need to count the survivors).
      {
        char key[16];
        int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);
        bson_t stageDoc, geoNearDoc;
        bson_append_document_begin(&stages, key, keyLen, &stageDoc);
        bson_append_document_begin(&stageDoc, "$geoNear", 8, &geoNearDoc);

        bson_t nearGeometry;
        bson_init(&nearGeometry);
        bson_append_utf8(&nearGeometry, "type", 4, filterP->geometry, -1);
        const char* coordStr = filterP->coordinates;
        while (*coordStr == ' ') coordStr++;
        if (*coordStr == '[')
          bsonAppendCoordArray(&nearGeometry, "coordinates", 11, coordStr);
        bson_append_document(&geoNearDoc, "near", 4, &nearGeometry);
        bson_destroy(&nearGeometry);

        bson_append_utf8(&geoNearDoc, "distanceField", 13, "geoDistance", 11);
        bson_append_utf8(&geoNearDoc, "key", 3, fieldPath, -1);
        bson_append_bool(&geoNearDoc, "spherical", 9, true);

        if (filterP->geoRel->maxDistance >= 0)
          bson_append_double(&geoNearDoc, "maxDistance", 11, filterP->geoRel->maxDistance);
        if (filterP->geoRel->minDistance >= 0)
          bson_append_double(&geoNearDoc, "minDistance", 11, filterP->geoRel->minDistance);

        bson_append_document_end(&stageDoc, &geoNearDoc);
        bson_append_document_end(&stages, &stageDoc);
      }

      // $count stage — emits a single document { n: <total> }.
      {
        char key[16];
        int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);
        bson_t stageDoc;
        bson_append_document_begin(&stages, key, keyLen, &stageDoc);
        bson_append_utf8(&stageDoc, "$count", 6, "n", 1);
        bson_append_document_end(&stages, &stageDoc);
      }

      bson_append_array_end(&pipeline, &stages);

      mongoc_cursor_t* countCursor = mongoc_collection_aggregate(collP, MONGOC_QUERY_NONE, &pipeline, NULL, NULL);
      bson_destroy(&pipeline);

      filterP->totalCount = 0;
      const bson_t* cdoc;
      if (mongoc_cursor_next(countCursor, &cdoc))
      {
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, cdoc, "n") && BSON_ITER_HOLDS_NUMBER(&iter))
          filterP->totalCount = bson_iter_as_int64(&iter);
      }
      bson_error_t cerr;
      if (mongoc_cursor_error(countCursor, &cerr))
        KT_E("mongoc: $geoNear count aggregation failed: %s", cerr.message);
      mongoc_cursor_destroy(countCursor);
    }
    else
    {
      bson_error_t countError;
      int64_t      total = mongoc_collection_count_documents(collP, &filter, NULL, NULL, NULL, &countError);

      if (total < 0)
      {
        KT_E("mongoc: count_documents failed: %s", countError.message);
        filterP->totalCount = 0;
      }
      else
      {
        filterP->totalCount = total;
      }
    }
  }

  //
  // For count-only mode, skip the query entirely and return an empty array
  //
  if (countOnly)
  {
    *arrayPP = kjArray(corRest.kjsonP, NULL);
    bson_destroy(&opts);
    bson_destroy(&filter);
    mongoc_collection_destroy(collP);
    mongoc_client_pool_push(poolP, clientP);
    return DB_OK;
  }

  //
  // For georel=near, use $geoNear aggregation pipeline to get distance.
  // For all other queries, use find().
  //
  bool useGeoNear  = (filterP != NULL && filterP->geoRel != NULL && filterP->geoRel->rel == LdGeoNear);
  bool useDistSort = (filterP != NULL && filterP->geoRel == NULL && filterP->distGeoproperty != NULL);

  mongoc_cursor_t* cursorP;

  if (useGeoNear || useDistSort)
  {
    //
    // Build aggregation pipeline: $geoNear -> $match [-> $sort -> $unionWith] -> $skip -> $limit
    //
    // $geoNear must be the first stage. It handles the geo reference and adds a
    // "geoDistance" field (metres) to each document. § 7.6.2.2 sort-by-distance
    // reuses this with the orderFrom Point as the reference, no distance cutoff,
    // then appends the entities that lack the GeoProperty ($unionWith, ranked
    // last) and paginates across both.
    //
    const char* nearGeoprop  = useDistSort ? filterP->distGeoproperty : filterP->geoproperty;
    const char* nearGeomType = useDistSort ? "Point"                  : filterP->geometry;
    const char* nearCoords   = useDistSort ? filterP->distFrom        : filterP->coordinates;

    char fieldPath[1024];
    snprintf(fieldPath, sizeof(fieldPath), "%s.@none.value", mongocEscapeDotsInKey(nearGeoprop));

    bson_t pipeline;
    bson_init(&pipeline);

    bson_t stages;
    bson_append_array_begin(&pipeline, "pipeline", 8, &stages);
    int stageIx = 0;

    // --- $geoNear stage ---
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);

      bson_t stageDoc;
      bson_t geoNearDoc;

      bson_append_document_begin(&stages, key, keyLen, &stageDoc);
      bson_append_document_begin(&stageDoc, "$geoNear", 8, &geoNearDoc);

      // Build reference geometry inline
      bson_t nearGeometry;
      bson_init(&nearGeometry);
      bson_append_utf8(&nearGeometry, "type", 4, nearGeomType, -1);

      const char* coordStr = nearCoords;
      while (*coordStr == ' ') coordStr++;
      if (*coordStr == '[')
        bsonAppendCoordArray(&nearGeometry, "coordinates", 11, coordStr);

      bson_append_document(&geoNearDoc, "near", 4, &nearGeometry);
      bson_destroy(&nearGeometry);

      bson_append_utf8(&geoNearDoc, "distanceField", 13, "geoDistance", 11);
      bson_append_utf8(&geoNearDoc, "key", 3, fieldPath, -1);
      bson_append_bool(&geoNearDoc, "spherical", 9, true);

      // dist-sort has no distance cutoff (geoRel is NULL).
      if (useGeoNear && filterP->geoRel->maxDistance >= 0)
        bson_append_double(&geoNearDoc, "maxDistance", 11, filterP->geoRel->maxDistance);
      if (useGeoNear && filterP->geoRel->minDistance >= 0)
        bson_append_double(&geoNearDoc, "minDistance", 11, filterP->geoRel->minDistance);

      bson_append_document_end(&stageDoc, &geoNearDoc);
      bson_append_document_end(&stages, &stageDoc);
    }

    // --- $match stage (non-geo filters) ---
    // The filter was built with the $near clause too, but $geoNear handles that.
    // Rebuild a filter without the geo part for $match.
    {
      bson_t matchFilter;
      bson_init(&matchFilter);

      bsonAppendNonGeoMatch(&matchFilter, filterP);

      // Only add $match if there are non-geo filters
      if (!bson_empty(&matchFilter))
      {
        char key[16];
        int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);
        bson_t stageDoc;
        bson_append_document_begin(&stages, key, keyLen, &stageDoc);
        bson_append_document(&stageDoc, "$match", 6, &matchFilter);
        bson_append_document_end(&stages, &stageDoc);
      }
      bson_destroy(&matchFilter);
    }

    // --- dist-sort tail: reverse to farthest-first for dist-desc, then append
    //     the entities that lack the GeoProperty ($unionWith), ranked last.
    //     $geoNear already yields nearest-first, so dist-asc needs no $sort. ---
    if (useDistSort)
    {
      if (filterP->distDesc)
      {
        char key[16];
        int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);
        bson_t stageDoc, sortDoc;
        bson_append_document_begin(&stages, key, keyLen, &stageDoc);
        bson_append_document_begin(&stageDoc, "$sort", 5, &sortDoc);
        BSON_APPEND_INT32(&sortDoc, "geoDistance", -1);
        bson_append_document_end(&stageDoc, &sortDoc);
        bson_append_document_end(&stages, &stageDoc);
      }

      // $unionWith: the entities matching the same non-geo filters but WITHOUT
      // the GeoProperty — appended after the distance-sorted ones, so they rank
      // last (§ 7.6.2.2) while $skip/$limit still page across the whole stream.
      {
        char key[16];
        int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);
        bson_t stageDoc, unionDoc, subPipe, subStage, subMatch, existsDoc;

        bson_append_document_begin(&stages, key, keyLen, &stageDoc);
        bson_append_document_begin(&stageDoc, "$unionWith", 10, &unionDoc);
        bson_append_utf8(&unionDoc, "coll", 4, "entities", 8);
        bson_append_array_begin(&unionDoc, "pipeline", 8, &subPipe);
        bson_append_document_begin(&subPipe, "0", 1, &subStage);
        bson_append_document_begin(&subStage, "$match", 6, &subMatch);

        bsonAppendNonGeoMatch(&subMatch, filterP);
        bson_append_document_begin(&subMatch, fieldPath, -1, &existsDoc);
        bson_append_bool(&existsDoc, "$exists", 7, false);
        bson_append_document_end(&subMatch, &existsDoc);

        bson_append_document_end(&subStage, &subMatch);
        bson_append_document_end(&subPipe, &subStage);
        bson_append_array_end(&unionDoc, &subPipe);
        bson_append_document_end(&stageDoc, &unionDoc);
        bson_append_document_end(&stages, &stageDoc);
      }
    }

    // --- $skip stage ---
    if (filterP->offset > 0)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);
      bson_t stageDoc;
      bson_append_document_begin(&stages, key, keyLen, &stageDoc);
      BSON_APPEND_INT64(&stageDoc, "$skip", filterP->offset);
      bson_append_document_end(&stages, &stageDoc);
    }

    // --- $limit stage ---
    if (filterP->limit > 0)
    {
      char key[16];
      int  keyLen = snprintf(key, sizeof(key), "%d", stageIx++);
      bson_t stageDoc;
      bson_append_document_begin(&stages, key, keyLen, &stageDoc);
      BSON_APPEND_INT64(&stageDoc, "$limit", filterP->limit);
      bson_append_document_end(&stages, &stageDoc);
    }

    bson_append_array_end(&pipeline, &stages);

    cursorP = mongoc_collection_aggregate(collP, MONGOC_QUERY_NONE, &pipeline, NULL, NULL);
    bson_destroy(&pipeline);
  }
  else
  {
    cursorP = mongoc_collection_find_with_opts(collP, &filter, &opts, NULL);
  }

  //
  // Build result array
  //
  KjNode*       arrayP = kjArray(corRest.kjsonP, NULL);
  const bson_t* doc;

  while (mongoc_cursor_next(cursorP, &doc))
  {
    KjNode* entityP = mongocBsonToKjTree(&corRest.kalloc, doc);
    kjChildAdd(arrayP, entityP);
  }

  //
  // Check for cursor error
  //
  bson_error_t error;
  int          result = DB_OK;

  if (mongoc_cursor_error(cursorP, &error))
  {
    KT_E("mongoc: entityQuery failed: %s", error.message);
    // mongo's PCRE regex validator rejects patterns POSIX regcomp
    // accepted (e.g. `**`). Distinguish bad-user-input (400) from
    // genuine 500-class errors so the service routine doesn't have
    // to know storage-engine specifics.
    if (strstr(error.message, "Regular expression is invalid") != NULL)
    {
      result             = DB_BAD_INPUT;
      filterP->errStatus = 400;
      filterP->errType   = LD_ERROR_BAD_REQUEST_DATA;
      filterP->errTitle  = "Invalid Query";
      snprintf(filterP->errDetail, sizeof(filterP->errDetail), "%s", error.message);
    }
    else
    {
      result             = DB_ERR;
      filterP->errStatus = 500;
      filterP->errType   = LD_ERROR_INTERNAL_ERROR;
      filterP->errTitle  = "Internal Error";
      snprintf(filterP->errDetail, sizeof(filterP->errDetail), "%s", error.message);
    }
  }
  else
  {
    *arrayPP = arrayP;
  }

  bson_destroy(&opts);
  bson_destroy(&filter);
  mongoc_cursor_destroy(cursorP);
  mongoc_collection_destroy(collP);
  mongoc_client_pool_push(poolP, clientP);

  return result;
}
