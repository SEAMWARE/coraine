#ifndef SERVICEROUTINES_DELETEENTITYTEMPORALATTR_H_
#define SERVICEROUTINES_DELETEENTITYTEMPORALATTR_H_

//
// FILE            deleteEntityTemporalAttr.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// DELETE /ngsi-ld/v1/temporal/entities/{id}/attrs/{attr} — § 5.6.13 / § 6.21.3.1.
//

#include <stdbool.h>

extern bool deleteEntityTemporalAttr(void);

#endif  // SERVICEROUTINES_DELETEENTITYTEMPORALATTR_H_
