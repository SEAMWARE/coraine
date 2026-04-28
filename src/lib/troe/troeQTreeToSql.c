//
// FILE            troeQTreeToSql.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stddef.h>                                   // NULL
#include <stdio.h>                                    // snprintf
#include <string.h>                                   // strlen, memcpy, strchr, strrchr
#include <stdbool.h>                                  // bool

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "swNgsild/LdQ.h"                             // LdQNode

#include "troe/troeQTreeToSql.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// opSqlSymbol - LdQOperator → SQL operator string. NULL if unsupported.
//
static const char* opSqlSymbol(LdQOperator op)
{
  switch (op)
  {
    case LdQEqual:      return "=";
    case LdQUnequal:    return "<>";
    case LdQGreater:    return ">";
    case LdQLess:       return "<";
    case LdQGreaterEq:  return ">=";
    case LdQLessEq:     return "<=";
    default:            return NULL;  // pattern, etc.
  }
}



// -----------------------------------------------------------------------------
//
// escapeSqlLit - copy s to dst escaping single quotes as ''. Returns chars written.
//
static int escapeSqlLit(const char* s, char* dst, int max)
{
  int n = 0;
  for (const char* p = s; *p && n < max - 2; p++)
  {
    if (*p == '\'') { dst[n++] = '\''; dst[n++] = '\''; }
    else            dst[n++] = *p;
  }
  dst[n] = 0;
  return n;
}



// -----------------------------------------------------------------------------
//
// termToSql - one LdQTermNode → "EXISTS (SELECT 1 FROM troe_attrs WHERE
// entity_id = $1 AND attr_name = '<iri>' AND <vCol> <op> <lit>)".
//
// Returns NULL on unsupported value type / op / sub-path.
//
static const char* termToSql(LdQTerm* tP, KAlloc* allocP)
{
  // Sub-attribute paths (a.b > 10) — skip for now.
  // The expanded IRI itself contains dots ("etsi.org/..."); look only in
  // the last path segment.
  if (tP->attr != NULL)
  {
    const char* tail = strrchr(tP->attr, '/');
    tail = (tail != NULL) ? tail + 1 : tP->attr;
    if (strchr(tail, '.') != NULL)
      return NULL;
  }

  // Existence-only check (no operator).
  if (tP->op == LdQExists && tP->valueType == LdQNoValue)
  {
    char attrEsc[1024];
    escapeSqlLit(tP->attr, attrEsc, sizeof(attrEsc));

    int   sz  = (int) strlen(attrEsc) + 128;
    char* buf = (char*) kaAlloc(allocP, sz);
    snprintf(buf, sz,
             "EXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = '%s')",
             attrEsc);
    return buf;
  }

  const char* opSym = opSqlSymbol(tP->op);
  if (opSym == NULL)
    return NULL;

  char  attrEsc[1024];
  escapeSqlLit(tP->attr, attrEsc, sizeof(attrEsc));

  // Pick value column + literal based on value type.
  const char* vCol = NULL;
  char        lit[256];
  bool        haveLit = false;

  if (tP->valueType == LdQNumber)
  {
    vCol = "v_number";
    snprintf(lit, sizeof(lit), "%.17g", tP->value.n);
    haveLit = true;
  }
  else if (tP->valueType == LdQString)
  {
    vCol = "v_text";
    char esc[256];
    escapeSqlLit(tP->value.s, esc, sizeof(esc));
    snprintf(lit, sizeof(lit), "'%s'", esc);
    haveLit = true;
  }
  else if (tP->valueType == LdQBool)
  {
    vCol = "v_bool";
    snprintf(lit, sizeof(lit), "%s", tP->value.b ? "TRUE" : "FALSE");
    haveLit = true;
  }
  else
  {
    return NULL;  // datetime / range / list — follow-up
  }

  if (!haveLit) return NULL;

  int   sz  = (int) strlen(attrEsc) + (int) strlen(vCol) + (int) strlen(lit) + 128;
  char* buf = (char*) kaAlloc(allocP, sz);
  snprintf(buf, sz,
           "EXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = '%s' AND %s %s %s)",
           attrEsc, vCol, opSym, lit);
  return buf;
}



// -----------------------------------------------------------------------------
//
// nodeToSql - recursive walker.
//
static const char* nodeToSql(LdQNode* qP, KAlloc* allocP)
{
  if (qP == NULL) return NULL;

  if (qP->type == LdQTermNode)
    return termToSql(&qP->term, allocP);

  if (qP->type == LdQAndNode || qP->type == LdQOrNode)
  {
    if (qP->group.count == 0) return NULL;

    // Recurse children, joining with AND/OR.
    const char* sep = (qP->type == LdQAndNode) ? " AND " : " OR ";

    // Compile children first to know total size.
    const char** parts = (const char**) kaAlloc(allocP, sizeof(char*) * qP->group.count);
    int totalLen = 4;  // "(" + ")" + slack
    for (int i = 0; i < qP->group.count; i++)
    {
      parts[i] = nodeToSql(qP->group.childV[i], allocP);
      if (parts[i] == NULL) return NULL;  // unsupported child → bail
      totalLen += (int) strlen(parts[i]) + (int) strlen(sep);
    }

    char* buf = (char*) kaAlloc(allocP, totalLen);
    int   p   = 0;
    buf[p++] = '(';
    for (int i = 0; i < qP->group.count; i++)
    {
      if (i > 0)
      {
        memcpy(buf + p, sep, strlen(sep));
        p += (int) strlen(sep);
      }
      int len = (int) strlen(parts[i]);
      memcpy(buf + p, parts[i], len);
      p += len;
    }
    buf[p++] = ')';
    buf[p]   = 0;
    return buf;
  }

  // LdQLinkedNode → not yet.
  return NULL;
}



// -----------------------------------------------------------------------------
//
// troeQTreeToSql -
//
const char* troeQTreeToSql(LdQNode* qTree, KAlloc* allocP)
{
  return nodeToSql(qTree, allocP);
}
