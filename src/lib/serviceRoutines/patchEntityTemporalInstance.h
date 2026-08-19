#ifndef SERVICEROUTINES_PATCHENTITYTEMPORALINSTANCE_H_
#define SERVICEROUTINES_PATCHENTITYTEMPORALINSTANCE_H_

//
// FILE            patchEntityTemporalInstance.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// PATCH /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr}/{instance} —
// § 5.6.14 / § 6.22.3.1.
//

#include <stdbool.h>

extern bool patchEntityTemporalInstance(void);

#endif  // SERVICEROUTINES_PATCHENTITYTEMPORALINSTANCE_H_
