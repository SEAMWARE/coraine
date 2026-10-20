#ifndef SRC_LIB_BRIDGE_DATACHANNELWRITEBACK_H_
#define SRC_LIB_BRIDGE_DATACHANNELWRITEBACK_H_

//
// FILE            dataChannelWriteBack.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Write-back (doc/bridge-service-execution.md § 2): a client's write to an attribute a DataChannel with
// channelDirection "both" maps goes to the device first, as a Channel's does - the bridge is published a
// record fragment: the columns the entity's id template names, read back out of the entity id, and the
// written attribute's column. The broker stores the write only when the bridge takes it.
//
// Compiled in with COR_FEATURE_CHANNEL_WRITE_BACK.
//
#include <stdbool.h>                                  // bool

#include "db/Tenant.h"                                // Tenant
#include "bridge/DataChannel.h"                       // DataChannel
#include "bridge/RecordMap.h"                         // RecordEntity, RecordAttr



// -----------------------------------------------------------------------------
//
// dataChannelWriteBackTemplateOk - an id template whose columns can be read back out of an id: no two
// placeholders side by side ("{a}{b}" - where one ends is not in the id). NULL: ok, else why not
//
extern const char* dataChannelWriteBackTemplateOk(const char* templ);



// -----------------------------------------------------------------------------
//
// dataChannelWriteBackOf - the DataChannel that writes back attrName of entityId: channelDirection both,
// of the tenant, available, one of its entities' id templates fits the id and that entity maps the
// attribute. NULL: none
//
extern DataChannel* dataChannelWriteBackOf(Tenant* tenantP, const char* entityId, const char* attrName, RecordEntity** entityPP, RecordAttr** attrPP);



// -----------------------------------------------------------------------------
//
// dataChannelWriteBackRecord - the record fragment, JSON text, malloc'd: the id template's columns (strings,
// percent-decoded) and the attribute's column, valueJson
//
// @return NULL, *whyP set: the attribute cannot be written back (not a column's value), or the id does not
//         read back
//
extern char* dataChannelWriteBackRecord(RecordEntity* entityP, RecordAttr* attrP, const char* entityId, const char* valueJson, const char** whyP);

#endif  // SRC_LIB_BRIDGE_DATACHANNELWRITEBACK_H_
