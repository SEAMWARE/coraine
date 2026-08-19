#ifndef SR_POSTEXNOTIFICATION_H_
#define SR_POSTEXNOTIFICATION_H_

//
// FILE            postExNotification.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// POST /ngsi-ld/ex/v1/notifications/{parentSubId}
//
// Receiver for notifications produced by remote Context Sources that
// hold a derived (distributed) subscription created on this broker
// (NGSI-LD § 5.8.1.4). The handler maps the incoming notification
// back to its originating local sub and re-dispatches the body to
// that sub's notification.endpoint.uri.
//
#include <stdbool.h>                                  // bool

extern bool postExNotification(void);

#endif  // SR_POSTEXNOTIFICATION_H_
