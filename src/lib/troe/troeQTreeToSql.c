//
// FILE            troeQTreeToSql.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stddef.h>                                   // NULL
#include <stdio.h>                                    // snprintf
#include <time.h>                                      // gmtime_r, struct tm
#include <string.h>                                   // strlen, memcpy, strchr, strrchr
#include <stdbool.h>                                  // bool

#include "kalloc/kaAlloc.h"                          // kaAlloc

#include "corNgsild/LdQ.h"                             // LdQNode

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
// jsonbPath - render a JSONB path expression for a § 4.9 attrPath term.
//
// The row's own columns only describe the ATTRIBUTE. Everything below it lives
// in the sub_attrs JSONB, which holds each sub-attribute verbatim in NGSI-LD
// form:
//
//   {"accuracy": {"type": "Property", "value": 9},
//    "meta":     {"type": "Property", "value": {"x": {"y": 7}}}}
//
// So a.b       → sub_attrs -> 'b' -> 'value'
//    a.b.c     → sub_attrs -> 'b' -> 'c' -> 'value'    (c is a member of b)
//    a[x.y]    → v_compound -> 'x' -> 'y'              (into the attr's value)
//    a.b[x.y]  → sub_attrs -> 'b' -> 'value' -> 'x' -> 'y'
//
// Written with #> and a path array so a member name never has to be escaped
// into the operator chain. Returns the number of chars written, or -1 if the
// path does not fit.
//
static int jsonbPath(LdQTerm* tP, char* dst, int max)
{
  int  n    = 0;
  bool sub  = (tP->subPathN > 0);

  n += snprintf(dst + n, max - n, "%s #> '{", sub ? "sub_attrs" : "v_compound");

  // Sub-attribute segments, then "value" to step from the NGSI-LD node into
  // what it holds. A value path continues from there.
  for (int i = 0; i < tP->subPathN; i++)
    n += snprintf(dst + n, max - n, "%s\"%s\"", (i > 0) ? "," : "", tP->subPathV[i]);

  if (sub)
    n += snprintf(dst + n, max - n, ",\"value\"");

  for (int i = 0; i < tP->valuePathN; i++)
    n += snprintf(dst + n, max - n, "%s\"%s\"", (i > 0 || sub) ? "," : "", tP->valuePathV[i]);

  n += snprintf(dst + n, max - n, "}'");

  return (n >= max) ? -1 : n;
}



// -----------------------------------------------------------------------------
//
// termToSql - one LdQTermNode → "EXISTS (SELECT 1 FROM troe_attrs WHERE
// entity_id = $1 AND attr_name = '<iri>' AND <lhs> <op> <lit>)".
//
// Returns NULL when the term cannot be expressed in SQL. The caller must then
// refuse the query rather than run it unfiltered — an ignored filter answers
// with entities that do not match, which is worse than an error.
//
static const char* termToSql(LdQTerm* tP, KAlloc* allocP)
{
  char attrEsc[1024];
  escapeSqlLit(tP->attr, attrEsc, sizeof(attrEsc));

  bool deep = (tP->subPathN > 0) || (tP->valuePathN > 0);

  //
  // observedAt is NOT in sub_attrs — timescaleEvent.c lifts it into its own
  // column on the way in. A path of exactly ".observedAt" is therefore a
  // column comparison, and the only one of the timestamps that q can reach:
  // createdAt/modifiedAt/datasetId are dropped from sub_attrs the same way,
  // but § 4.9 does not put them in an attrPath.
  //
  bool observedAtPath = (tP->subPathN == 1) && (tP->valuePathN == 0) &&
                        (strcmp(tP->subPathV[0], "observedAt") == 0);

  // Existence / non-existence check (no operator).
  if ((tP->op == LdQExists || tP->op == LdQNotExists) && tP->valueType == LdQNoValue)
  {
    char  pathBuf[1024] = { 0 };
    if (deep && !observedAtPath)
    {
      if (jsonbPath(tP, pathBuf, sizeof(pathBuf)) < 0)
        return NULL;
    }

    int   sz  = (int) strlen(attrEsc) + (int) strlen(pathBuf) + 192;
    char* buf = (char*) kaAlloc(allocP, sz);

    if (observedAtPath)
      snprintf(buf, sz,
               "%sEXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = '%s'"
               " AND observed_at IS NOT NULL)",
               (tP->op == LdQNotExists) ? "NOT " : "", attrEsc);
    else if (deep)
      snprintf(buf, sz,
               "%sEXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = '%s'"
               " AND %s IS NOT NULL)",
               (tP->op == LdQNotExists) ? "NOT " : "", attrEsc, pathBuf);
    else
      snprintf(buf, sz,
               "%sEXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = '%s')",
               (tP->op == LdQNotExists) ? "NOT " : "", attrEsc);
    return buf;
  }

  //
  // Left-hand side.
  //
  // A plain attribute compares against the typed column that its value was
  // stored in. Anything deeper has to come back out of JSONB, and JSONB is
  // untyped until it is cast — so each cast is guarded by jsonb_typeof(), both
  // to make the comparison mean what it says and to keep a cast from erroring
  // on a row where that member happens to hold something else.
  //
  char lhs[1200];
  char guard[1200];

  guard[0] = 0;

  if (observedAtPath)
    snprintf(lhs, sizeof(lhs), "observed_at");
  else if (deep)
  {
    char pathBuf[1024];
    if (jsonbPath(tP, pathBuf, sizeof(pathBuf)) < 0)
      return NULL;

    const char* jType = NULL;
    const char* cast  = NULL;

    switch (tP->valueType)
    {
      case LdQNumber:
      case LdQRange:      jType = "number";  cast = "::double precision"; break;
      case LdQString:     jType = "string";  cast = "";                   break;
      case LdQBool:       jType = "boolean"; cast = "::boolean";          break;
      case LdQDateTime:
      case LdQDateRange:  jType = "string";  cast = "::timestamptz";      break;
      case LdQValueList:  jType = NULL;      cast = NULL;                 break;   // per-item below
      default:            return NULL;
    }

    if (tP->valueType == LdQValueList)
    {
      // The list decides its own type; jsonb_typeof is applied per item type
      // in the list branch, which needs the raw text either way.
      snprintf(lhs, sizeof(lhs), "(%s #>> '{}')", pathBuf);
      (void) jType;
    }
    else
    {
      snprintf(guard, sizeof(guard), "jsonb_typeof(%s) = '%s' AND ", pathBuf, jType);
      snprintf(lhs, sizeof(lhs), "(%s #>> '{}')%s", pathBuf, cast);
    }
  }
  else
  {
    switch (tP->valueType)
    {
      case LdQNumber:
      case LdQRange:      snprintf(lhs, sizeof(lhs), "v_number");   break;
      case LdQString:     snprintf(lhs, sizeof(lhs), "v_text");     break;
      case LdQBool:       snprintf(lhs, sizeof(lhs), "v_bool");     break;
      case LdQDateTime:
      case LdQDateRange:  snprintf(lhs, sizeof(lhs), "v_datetime"); break;
      case LdQValueList:  snprintf(lhs, sizeof(lhs), "%s", "");     break;   // set in the list branch
      default:            return NULL;
    }
  }

  //
  // Right-hand side + operator.
  //
  char cond[4096];

  if ((tP->valueType == LdQRange) || (tP->valueType == LdQDateRange))
  {
    // § 4.9: a range is only meaningful for equality and its negation.
    if ((tP->op != LdQEqual) && (tP->op != LdQUnequal))
      return NULL;

    const char* neg = (tP->op == LdQUnequal) ? "NOT " : "";

    if (tP->valueType == LdQRange)
      snprintf(cond, sizeof(cond), "%s%s BETWEEN %.17g AND %.17g",
               neg, lhs, tP->value.numRange.lo, tP->value.numRange.hi);
    else
    {
      char lo[256], hi[256];
      escapeSqlLit(tP->value.dateRange.lo, lo, sizeof(lo));
      escapeSqlLit(tP->value.dateRange.hi, hi, sizeof(hi));
      snprintf(cond, sizeof(cond), "%s%s BETWEEN '%s'::timestamptz AND '%s'::timestamptz",
               neg, lhs, lo, hi);
    }
  }
  else if (tP->valueType == LdQValueList)
  {
    if ((tP->op != LdQEqual) && (tP->op != LdQUnequal))
      return NULL;

    // Every item is rendered as text and compared against the column that
    // matches the list's item type, so "speed==10,77" stays numeric and
    // "name=='a','b'" stays textual.
    const char* col  = NULL;
    bool        quot = false;

    switch (tP->value.list.itemType)
    {
      case LdQNumber:   col = deep ? lhs : "v_number";   quot = false; break;
      case LdQString:   col = deep ? lhs : "v_text";     quot = true;  break;
      case LdQBool:     col = deep ? lhs : "v_bool";     quot = false; break;
      case LdQDateTime: col = deep ? lhs : "v_datetime"; quot = true;  break;
      default:          return NULL;
    }

    int p = snprintf(cond, sizeof(cond), "%s%s IN (",
                     (tP->op == LdQUnequal) ? "NOT " : "", col);

    for (int i = 0; i < tP->value.list.count; i++)
    {
      char esc[256];
      escapeSqlLit(tP->value.list.values[i], esc, sizeof(esc));

      if (quot)
        p += snprintf(cond + p, sizeof(cond) - p, "%s'%s'%s", (i > 0) ? "," : "", esc,
                      (tP->value.list.itemType == LdQDateTime) ? "::timestamptz" : "");
      else
        p += snprintf(cond + p, sizeof(cond) - p, "%s%s", (i > 0) ? "," : "", esc);

      if (p >= (int) sizeof(cond) - 4)
        return NULL;
    }
    snprintf(cond + p, sizeof(cond) - p, ")");
  }
  else if ((tP->op == LdQPattern) || (tP->op == LdQNotPattern))
  {
    // ~= is a POSIX regular expression, which is postgres' own ~ operator.
    char esc[512];
    escapeSqlLit(tP->value.s, esc, sizeof(esc));
    snprintf(cond, sizeof(cond), "%s %s '%s'", lhs, (tP->op == LdQPattern) ? "~" : "!~", esc);
  }
  else
  {
    const char* opSym = opSqlSymbol(tP->op);
    if (opSym == NULL)
      return NULL;

    char lit[512];

    switch (tP->valueType)
    {
      case LdQNumber:   snprintf(lit, sizeof(lit), "%.17g", tP->value.n); break;
      case LdQBool:     snprintf(lit, sizeof(lit), "%s", tP->value.b ? "TRUE" : "FALSE"); break;

      case LdQString:
      {
        char esc[256];
        escapeSqlLit(tP->value.s, esc, sizeof(esc));
        snprintf(lit, sizeof(lit), "'%s'", esc);
        break;
      }

      case LdQDateTime:
      {
        // Held as nanoseconds since the epoch; postgres wants an ISO literal.
        long long  ns   = tP->value.ns;
        time_t     secs = (time_t) (ns / 1000000000LL);
        long       rest = (long) (ns % 1000000000LL);
        struct tm  tmv;

        gmtime_r(&secs, &tmv);
        snprintf(lit, sizeof(lit), "'%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ'::timestamptz",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, rest / 1000);
        break;
      }

      default: return NULL;
    }

    snprintf(cond, sizeof(cond), "%s %s %s", lhs, opSym, lit);
  }

  int   sz  = (int) strlen(attrEsc) + (int) strlen(guard) + (int) strlen(cond) + 128;
  char* buf = (char*) kaAlloc(allocP, sz);
  snprintf(buf, sz,
           "EXISTS (SELECT 1 FROM troe_attrs WHERE entity_id = $1 AND attr_name = '%s' AND %s%s)",
           attrEsc, guard, cond);
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
