//
// FILE            troeNotAvailable.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#ifndef SRC_LIB_TROE_TROENOTAVAILABLE_H_
#define SRC_LIB_TROE_TROENOTAVAILABLE_H_



// -----------------------------------------------------------------------------
//
// troeNotAvailable - emit a uniform 501 ProblemDetails when a temporal
//                    endpoint is hit on a broker started without TRoE
//                    support (no plugin loaded, or `--troe none`).
//
//                    `op` is a short operation label included in the
//                    detail message (e.g. "temporal-entity create").
//
extern void troeNotAvailable(const char* op);

#endif  // SRC_LIB_TROE_TROENOTAVAILABLE_H_
