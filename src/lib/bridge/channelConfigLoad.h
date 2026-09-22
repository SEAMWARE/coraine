#ifndef BRIDGE_CHANNELCONFIGLOAD_H_
#define BRIDGE_CHANNELCONFIGLOAD_H_

//
// FILE            channelConfigLoad.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// channelConfigLoad - create Channels from a bridge configuration file
//
// For every loaded bridge plugin, reads <alias>.ngsild.topics from the file and
// creates one Channel per entry.
//
// The file's shape is fixed by deployments that already use it, so it is read
// as it stands rather than reshaped:
//
//   {
//     "dds": {
//       "ddsmodule": { ...the transport's own settings, not read here... },
//       "ngsild": {
//         "topics": {
//           "rt/pose": { "entityId": "urn:...", "entityType": "Robot", "attribute": "pose" }
//         }
//       }
//     }
//   }
//
// The top-level key is the bridge alias, which is what makes one file able to
// describe several transports without the format changing.
//
// @param path       the file. NULL means the default location, whose absence is
//                   not an error - a bridge may need no configuration at all.
// @param explicitly true when the path came from the command line, which makes
//                   an unreadable file fatal rather than merely absent.
// @param tenantP    the tenant these Channels write into.
//
// @return the number of Channels created, or -1 if the file could not be used.
//
// ⚠ Does not return on a malformed file: a duplicate endpoint or a collision on
// a target attribute ends startup. See the note in the .c on why those two are
// fatal while an incomplete entry is only skipped.
//
extern int channelConfigLoad(const char* path, bool explicitly, Tenant* tenantP);

#endif  // BRIDGE_CHANNELCONFIGLOAD_H_
