//
// FILE            adminMetrics.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /admin/metrics — thin wrapper over metrics.c's renderer so the
// admin plugin doesn't need to know about kprom.
//
#include "metrics/metrics.h"                      // metricsRender

#include "api/admin/adminMetrics.h"               // Own interface



bool adminGetMetrics(void)
{
  return metricsRender();
}
