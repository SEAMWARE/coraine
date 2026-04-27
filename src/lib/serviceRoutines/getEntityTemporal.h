#ifndef SERVICEROUTINES_GETENTITYTEMPORAL_H_
#define SERVICEROUTINES_GETENTITYTEMPORAL_H_

//
// FILE            getEntityTemporal.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/temporal/entities/{id}
// NGSI-LD § 5.7.4 — Retrieve Temporal Evolution of an Entity.
//

#include <stdbool.h>

extern bool getEntityTemporal(void);

#endif  // SERVICEROUTINES_GETENTITYTEMPORAL_H_
