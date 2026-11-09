#ifndef SERVICEROUTINES_GETENTITIESTEMPORAL_H_
#define SERVICEROUTINES_GETENTITIESTEMPORAL_H_

//
// FILE            getEntitiesTemporal.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/temporal/entities
// NGSI-LD § 5.7.4 — Query Temporal Evolution of Entities (§ 6.18.3.2).
//

#include <stdbool.h>

extern bool getEntitiesTemporal(void);



#if COR_FEATURE_TEMPORAL_ENTITY_MAP
// -----------------------------------------------------------------------------
//
// createTemporalEntityMap - GET /temporal/entityMaps (TS 104-176 § 11.3.3.1): the EntityMap of a temporal
// query, 201 and NGSILD-EntityMap
//
extern bool createTemporalEntityMap(void);
#endif

#endif  // SERVICEROUTINES_GETENTITIESTEMPORAL_H_
