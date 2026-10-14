#ifndef BRIDGE_DATACHANNELRECORDIN_H_
#define BRIDGE_DATACHANNELRECORDIN_H_

//
// FILE            dataChannelRecordIn.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdint.h>                                   // int64_t

#include "bridge/DataChannel.h"                       // DataChannel



// -----------------------------------------------------------------------------
//
// dataChannelRecordIn - a record on a DataChannel: the entities its mapping makes of it, upserted
//
// The record is a JSON object, one member per column. Each entity of the mapping (RecordMap.h) whose id the
// record can fill in is made, with the attributes the record has values for, and all of them are written as
// ONE batch upsert with options=update: an entity that is not there is created, an attribute that is there
// is replaced, and the attributes the mapping does not name are left alone. The write is the one POST
// /ngsi-ld/v1/entityOperations/upsert does - the same checks, notifications and temporal events, each
// entity on its own (one refused entity does not refuse the others) - on the DataChannel's tenant, local.
//
// publishTime (nanoseconds) is each attribute's observedAt; 0 (the bridge said no time): no observedAt.
//
// On the plugin's thread, from bridgeSampleIn.
//
// @return BRIDGE_OK (at least one entity written), BRIDGE_BAD_INPUT (not a JSON object, or a record that
//         makes no entity: a column an id needs has no value), BRIDGE_ERR (nothing written - the reasons
//         logged)
//
extern int dataChannelRecordIn(DataChannel* dcP, const char* json, int64_t publishTime);

#endif  // BRIDGE_DATACHANNELRECORDIN_H_
