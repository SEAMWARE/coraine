#ifndef ADMIN_ADMIN_METRICS_H_
#define ADMIN_ADMIN_METRICS_H_

//
// FILE            adminMetrics.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /admin/metrics — Prometheus-format metrics exposition.
// Content-Type: text/plain; version=0.0.4.
//
#include <stdbool.h>

extern bool adminGetMetrics(void);

#endif  // ADMIN_ADMIN_METRICS_H_
