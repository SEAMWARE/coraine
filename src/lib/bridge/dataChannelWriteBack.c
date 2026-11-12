//
// FILE            dataChannelWriteBack.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free, realloc
#include <string.h>                                   // strcmp, strlen, strchr, strncmp, memcpy
#include <pthread.h>                                  // pthread_mutex_*

#include "corHash/corHash.h"                          // CorHashTable, corHashTableCreate, corHashItemAdd, corHashItemLookup

#include "db/Tenant.h"                                // Tenant
#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/RecordMap.h"                         // RecordEntity, RecordAttr
#include "bridge/dataChannelCache.h"                  // dataChannelFirst
#include "bridge/dataChannelWriteBack.h"              // Own interface



//
// COLUMNS_MAX - the placeholders of one id template that are read back
//
#define COLUMNS_MAX  16



// -----------------------------------------------------------------------------
//
// Column - a placeholder's column and its value as read out of the id (percent-decoded)
//
typedef struct Column
{
  char  name[256];
  char  value[512];
} Column;



// -----------------------------------------------------------------------------
//
// dataChannelWriteBackTemplateOk -
//
const char* dataChannelWriteBackTemplateOk(const char* templ)
{
  for (const char* cP = templ; *cP != 0; cP++)
  {
    if ((*cP == '}') && (cP[1] == '{'))
      return "two placeholders side by side in its id template - where one ends cannot be read back out of an id";
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// hexVal -
//
static int hexVal(char c)
{
  if ((c >= '0') && (c <= '9')) return c - '0';
  if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
  if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
  return -1;
}



// -----------------------------------------------------------------------------
//
// idRead - the template's columns, read out of the id: literals match exactly, a placeholder takes what is
// up to the next literal (its first occurrence), or the rest. false: the id does not fit
//
static bool idRead(const char* templ, const char* id, Column* columnV, int* columnsP)
{
  const char* tP = templ;
  const char* iP = id;

  *columnsP = 0;

  while (*tP != 0)
  {
    if (*tP != '{')
    {
      if (*tP != *iP)
        return false;
      tP++;
      iP++;
      continue;
    }

    const char* endP = strchr(tP + 1, '}');           // there is one: the template was checked at load

    if ((endP == NULL) || (*columnsP >= COLUMNS_MAX) || (endP - tP - 1 >= (int) sizeof(columnV[0].name)))
      return false;

    Column* cP = &columnV[*columnsP];

    memcpy(cP->name, tP + 1, endP - tP - 1);
    cP->name[endP - tP - 1] = 0;

    //
    // The literal after the placeholder, up to the next one or the end
    //
    const char* litP   = endP + 1;
    const char* litEnd = strchr(litP, '{');
    int         litLen = (litEnd != NULL) ? (int) (litEnd - litP) : (int) strlen(litP);
    const char* valEnd;

    if (litLen == 0)
      valEnd = iP + strlen(iP);                       // the last placeholder: the rest of the id
    else
    {
      valEnd = iP;
      while ((*valEnd != 0) && (strncmp(valEnd, litP, litLen) != 0))
        valEnd++;
      if (*valEnd == 0)
        return false;
    }

    if (valEnd == iP)
      return false;                                   // a column is never empty in an id (recordTemplateExpand)

    //
    // Percent-decoded: the encoding the id was made with
    //
    int at = 0;

    for (const char* vP = iP; vP < valEnd; vP++)
    {
      if (at >= (int) sizeof(cP->value) - 1)
        return false;

      if ((*vP == '%') && (vP + 2 < valEnd) && (hexVal(vP[1]) >= 0) && (hexVal(vP[2]) >= 0))
      {
        cP->value[at++] = (char) ((hexVal(vP[1]) << 4) | hexVal(vP[2]));
        vP += 2;
      }
      else
        cP->value[at++] = *vP;
    }
    cP->value[at] = 0;

    (*columnsP)++;
    iP = valEnd;
    tP = endP + 1;
  }

  return (*iP == 0);
}



// -----------------------------------------------------------------------------
//
// Maker - the DataChannel whose records last wrote an entity. Two DataChannels' id templates can fit one id -
// two bridges each with a "*" DataChannel making "urn:ngsi-ld:Light:{@deviceId}" - and a write goes back to
// the one that makes the entity, not the first that fits (corHome 2026-11-12: a Tapo bulb's writes went to
// the matter bridge, which has no such node - 400).
//
// Kept for the DataChannels with channelDirection both only, one per entity they make, never removed: an
// entity deleted, or made by another DataChannel since, leaves an entry that is only ever compared with
// the DataChannels that fit - a DataChannel deleted is never chosen by it.
//
typedef struct Maker
{
  DataChannel*  dcP;
  char          key[];                                // the tenant's address and the entity id
} Maker;

static CorHashTable*    makerHash = NULL;
static pthread_mutex_t  makerLock = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// makerKey - the tenant's address and the entity id; false: the id is too long to be kept
//
static bool makerKey(Tenant* tenantP, const char* entityId, char* key, int size)
{
  int len = snprintf(key, size, "%p %s", (void*) tenantP, entityId);

  return (len > 0) && (len < size);
}



// -----------------------------------------------------------------------------
//
// makerHashCode - djb2
//
static unsigned int makerHashCode(const char* name)
{
  unsigned int hash = 5381;

  for (; *name != 0; name++)
    hash = ((hash << 5) + hash) + (unsigned char) *name;

  return hash;
}



// -----------------------------------------------------------------------------
//
// makerCompare -
//
static int makerCompare(const char* name, void* itemP)
{
  return strcmp(name, ((Maker*) itemP)->key);
}



// -----------------------------------------------------------------------------
//
// dataChannelWriteBackMade -
//
void dataChannelWriteBackMade(DataChannel* dcP, const char* entityId)
{
  char key[2048];

  if ((dcP->direction != BridgeDirectionBoth) || (makerKey(dcP->tenantP, entityId, key, sizeof(key)) == false))
    return;

  pthread_mutex_lock(&makerLock);

  if (makerHash == NULL)
    makerHash = corHashTableCreate(NULL, makerHashCode, makerCompare, 1024);

  if (makerHash != NULL)
  {
    Maker* makerP = (Maker*) corHashItemLookup(makerHash, key);

    if (makerP != NULL)
      makerP->dcP = dcP;
    else if ((makerP = malloc(sizeof(Maker) + strlen(key) + 1)) != NULL)
    {
      makerP->dcP = dcP;
      strcpy(makerP->key, key);

      if (corHashItemAdd(makerHash, makerP->key, makerP) != 0)
        free(makerP);
    }
  }

  pthread_mutex_unlock(&makerLock);
}



// -----------------------------------------------------------------------------
//
// makerOf - the DataChannel that last made the entity; NULL: none kept
//
static DataChannel* makerOf(Tenant* tenantP, const char* entityId)
{
  char         key[2048];
  DataChannel* dcP = NULL;

  if (makerKey(tenantP, entityId, key, sizeof(key)) == false)
    return NULL;

  pthread_mutex_lock(&makerLock);

  if (makerHash != NULL)
  {
    Maker* makerP = (Maker*) corHashItemLookup(makerHash, key);

    if (makerP != NULL)
      dcP = makerP->dcP;
  }

  pthread_mutex_unlock(&makerLock);

  return dcP;
}



// -----------------------------------------------------------------------------
//
// dataChannelWriteBackOf - of the DataChannels that fit, the entity's maker; none of them its maker: the first
//
DataChannel* dataChannelWriteBackOf(Tenant* tenantP, const char* entityId, const char* attrName, RecordEntity** entityPP, RecordAttr** attrPP)
{
  DataChannel*  makerP  = makerOf(tenantP, entityId);
  DataChannel*  firstP  = NULL;
  RecordEntity* firstEP = NULL;
  RecordAttr*   firstAP = NULL;

  for (DataChannel* dcP = dataChannelFirst(); dcP != NULL; dcP = dcP->next)
  {
    if ((dcP->direction != BridgeDirectionBoth) || (dcP->tenantP != tenantP))
      continue;

    if ((firstP != NULL) && (dcP != makerP))          // the first is known: only the maker can be better
      continue;

    for (RecordEntity* eP = dcP->mapP->entities; eP != NULL; eP = eP->next)
    {
      Column     columnV[COLUMNS_MAX];
      int        columns;
      RecordAttr* aP;

      if (idRead(eP->id, entityId, columnV, &columns) == false)
        continue;

      for (aP = eP->attrs; aP != NULL; aP = aP->next)
      {
        if (strcmp(aP->name, attrName) == 0)
          break;
      }

      if (aP == NULL)
        continue;

      if ((makerP == NULL) || (dcP == makerP))
      {
        *entityPP = eP;
        *attrPP   = aP;
        return dcP;
      }

      if (firstP == NULL)
      {
        firstP  = dcP;
        firstEP = eP;
        firstAP = aP;
      }
      break;
    }
  }

  if (firstP != NULL)
  {
    *entityPP = firstEP;
    *attrPP   = firstAP;
  }

  return firstP;
}



// -----------------------------------------------------------------------------
//
// jsonString - s as a JSON string, appended at *atP of out (size); false: no room
//
static bool jsonString(char* out, int size, int* atP, const char* s)
{
  int at = *atP;

  if (at >= size - 2)
    return false;
  out[at++] = '"';

  for (; *s != 0; s++)
  {
    unsigned char c = (unsigned char) *s;

    if (at >= size - 8)
      return false;

    if ((c == '"') || (c == '\\'))
    {
      out[at++] = '\\';
      out[at++] = c;
    }
    else if (c < 0x20)
      at += snprintf(&out[at], size - at, "\\u%04x", c);
    else
      out[at++] = c;
  }

  out[at++] = '"';
  out[at]   = 0;
  *atP      = at;
  return true;
}



// -----------------------------------------------------------------------------
//
// dataChannelWriteBackRecord -
//
char* dataChannelWriteBackRecord(RecordEntity* entityP, RecordAttr* attrP, const char* entityId, const char* valueJson, const char** whyP)
{
  if (attrP->kind != RecordAttrColumn)
  {
    *whyP = "it is not one column's value in the mapping - it cannot be written back";
    return NULL;
  }

  Column columnV[COLUMNS_MAX];
  int    columns;

  if (idRead(entityP->id, entityId, columnV, &columns) == false)
  {
    *whyP = "the entity id does not read back through the id template";
    return NULL;
  }

  int   size = 1024 + (int) strlen(valueJson);
  char* out  = malloc(size);
  int   at   = 0;

  if (out == NULL)
  {
    *whyP = "out of memory";
    return NULL;
  }

  out[at++] = '{';

  for (int ix = 0; ix < columns; ix++)
  {
    bool twice = (strcmp(columnV[ix].name, attrP->column) == 0);    // the written value says it

    for (int jx = 0; jx < ix; jx++)
    {
      if (strcmp(columnV[jx].name, columnV[ix].name) == 0)
        twice = true;
    }

    if (twice == true)
      continue;

    if ((at > 1) && (at < size - 1))
      out[at++] = ',';

    if ((jsonString(out, size, &at, columnV[ix].name) == false) || (at >= size - 1))
      goto noRoom;
    out[at++] = ':';
    if (jsonString(out, size, &at, columnV[ix].value) == false)
      goto noRoom;
  }

  if (at > 1)
    out[at++] = ',';

  if (jsonString(out, size, &at, attrP->column) == false)
    goto noRoom;

  int valueLen = strlen(valueJson);

  if (at + 1 + valueLen + 2 > size)
    goto noRoom;

  out[at++] = ':';
  memcpy(&out[at], valueJson, valueLen);
  at += valueLen;
  out[at++] = '}';
  out[at]   = 0;

  return out;

noRoom:
  free(out);
  *whyP = "the record fragment is too long";
  return NULL;
}
