#ifndef BRIDGE_CHANNELPREPOPULATE_H_
#define BRIDGE_CHANNELPREPOPULATE_H_

//
// FILE            channelPrePopulate.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include "db/Tenant.h"                                // Tenant



// -----------------------------------------------------------------------------
//
// channelPrePopulate - give every Channel somewhere to write
//
// A Channel names an entity and an attribute. Neither has to exist: the
// configuration describes what a transport will publish, not what the broker
// already holds, and on a fresh deployment it holds nothing.
//
// So each Channel's entity and attribute are created here if missing, the
// attribute carrying the placeholder value "uninitialized". The alternative is
// requiring every deployment to provision by hand the entities its own
// configuration already describes, which is the same information entered twice.
//
// ⭐ ONLY WHAT IS MISSING IS TOUCHED. An attribute that already exists is left
// exactly as it is - this runs on every start, and a restart that reset live
// values to "uninitialized" would lose data every time the broker came back.
//
// @return the number of attributes created, or -1 if the DB driver cannot say
//         what exists.
//
extern int channelPrePopulate(Tenant* tenantP);

#endif  // BRIDGE_CHANNELPREPOPULATE_H_
