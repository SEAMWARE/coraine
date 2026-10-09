#ifndef BRIDGE_RECORDMAP_FUNCTIONS_H_
#define BRIDGE_RECORDMAP_FUNCTIONS_H_

//
// FILE            recordMap.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode

#include "bridge/RecordMap.h"                         // RecordMap



// -----------------------------------------------------------------------------
//
// recordMapParse - a record Channel's entry of the configuration file -> its RecordMap
//
// @param entryP  the entry: { "entities": [ ... ], "channelInfo": [ ... ] } - channelInfo is the
//                caller's, the rest is read here
// @param kaP     scratch, for the expansion of names - nothing in the result points into it
// @param whyP    on NULL: why the entry cannot be used, one line (static text, or in kaP)
//
// @return the map, malloc'd (recordMapFree), or NULL
//
extern RecordMap* recordMapParse(CorNode* entryP, CorAlloc* kaP, const char** whyP);



// -----------------------------------------------------------------------------
//
// recordMapFree -
//
extern void recordMapFree(RecordMap* mapP);



// -----------------------------------------------------------------------------
//
// recordTemplateExpand - "urn:ngsi-ld:District:{District}" over a record
//
// Each "{Column}" is the column's value - a string with its characters outside [A-Za-z0-9._-]
// replaced by '_', a number as written, true/false. NULL when a column it names has no value in the
// record (absent, null, an empty string, an object or an array).
//
extern char* recordTemplateExpand(const char* templ, CorNode* recordP, CorAlloc* kaP);

#endif  // BRIDGE_RECORDMAP_FUNCTIONS_H_
