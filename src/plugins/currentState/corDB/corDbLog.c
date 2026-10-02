//
// FILE            corDbLog.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A record (doc/cordb-persistence.md § 4), little-endian:
//
//   0   4  'c' 'r' version op
//   4   4  body length
//   8   8  sequence
//   16  8  system time, ns
//   24  4  CRC-32C of bytes 0-23 and the body
//   28  n  body: the cor binary tree - the NGSI-LD codec, NO string tables, so it decodes on its own
//
#include <stdbool.h>                                   // bool
#include <stdint.h>                                    // uint32_t, uint64_t
#include <stdlib.h>                                    // realloc
#include <string.h>                                    // memcpy

#include "corBase/corCrc32c.h"                         // corCrc32c
#include "corTree/corTreeBin.h"                        // corTreeBinEncode, corTreeBinDecode
#include "corNgsild/ldBinCodec.h"                      // ldBinCodec

#include "corDbLog.h"                                  // Own interface

static const unsigned char VERSION = 1;



// -----------------------------------------------------------------------------
//
// room - make sure outP can take n more bytes
//
static bool room(CorBinBuffer* outP, int n)
{
  if (outP->len + n <= outP->size)
    return true;

  int   size = (outP->size == 0) ? 4096 : outP->size;
  while (size < outP->len + n)
    size *= 2;

  char* buf = realloc(outP->buf, size);
  if (buf == NULL)
    return false;

  outP->buf  = buf;
  outP->size = size;
  return true;
}



// -----------------------------------------------------------------------------
//
// corDbLogEncode -
//
bool corDbLogEncode(CorBinBuffer* outP, CorDbLogOp op, uint64_t seq, uint64_t sysTimeNs, CorNode* bodyP)
{
  int start = outP->len;

  if (room(outP, COR_DB_LOG_HEADER_LEN) == false)
    return false;

  outP->len += COR_DB_LOG_HEADER_LEN;               // the header is patched in after the body

  if ((bodyP != NULL) && (corTreeBinEncode(bodyP, &ldBinCodec, NULL, outP) == false))
  {
    outP->len = start;
    return false;
  }

  char*    h       = &outP->buf[start];
  uint32_t bodyLen = (uint32_t) (outP->len - start - COR_DB_LOG_HEADER_LEN);

  h[0] = 'c';
  h[1] = 'r';
  h[2] = (char) VERSION;
  h[3] = (char) op;
  memcpy(&h[4],  &bodyLen,   4);
  memcpy(&h[8],  &seq,       8);
  memcpy(&h[16], &sysTimeNs, 8);

  uint32_t crc = corCrc32c(0, h, 24);
  crc = corCrc32c(crc, &h[COR_DB_LOG_HEADER_LEN], bodyLen);
  memcpy(&h[24], &crc, 4);

  return true;
}



// -----------------------------------------------------------------------------
//
// corDbLogNext -
//
CorDbLogStatus corDbLogNext(const char* buf, int len, int* offsetP, CorAlloc* kaP, CorDbLogRecord* recP)
{
  int off = *offsetP;

  if (off >= len)
    return CorDbLogEnd;

  if (len - off < COR_DB_LOG_HEADER_LEN)
    return CorDbLogTorn;

  const char* h = &buf[off];
  uint32_t    bodyLen;
  uint32_t    crc;

  if ((h[0] != 'c') || (h[1] != 'r') || ((unsigned char) h[2] != VERSION))
    return CorDbLogTorn;

  memcpy(&bodyLen, &h[4], 4);
  if ((uint64_t) bodyLen > (uint64_t) (len - off - COR_DB_LOG_HEADER_LEN))
    return CorDbLogTorn;

  memcpy(&crc, &h[24], 4);
  if (corCrc32c(corCrc32c(0, h, 24), &h[COR_DB_LOG_HEADER_LEN], bodyLen) != crc)
    return CorDbLogTorn;

  recP->op = (CorDbLogOp) (unsigned char) h[3];
  memcpy(&recP->seq,       &h[8],  8);
  memcpy(&recP->sysTimeNs, &h[16], 8);
  recP->bodyP = NULL;

  if (bodyLen > 0)
  {
    const char* error = NULL;

    recP->bodyP = corTreeBinDecode(&h[COR_DB_LOG_HEADER_LEN], (int) bodyLen, &ldBinCodec, NULL, kaP, &error);
    if (recP->bodyP == NULL)
      return CorDbLogTorn;                           // a CRC-valid record that does not decode: a bug, not a torn write - stop all the same
  }

  *offsetP = off + COR_DB_LOG_HEADER_LEN + (int) bodyLen;
  return CorDbLogOk;
}
