#ifndef SERVICEROUTINES_DELETEENTITYTEMPORALINSTANCE_H_
#define SERVICEROUTINES_DELETEENTITYTEMPORALINSTANCE_H_

//
// FILE            deleteEntityTemporalInstance.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr}/{instance} —
// § 5.6.15 / § 6.22.3.2.
//

#include <stdbool.h>

extern bool deleteEntityTemporalInstance(void);

#endif  // SERVICEROUTINES_DELETEENTITYTEMPORALINSTANCE_H_
