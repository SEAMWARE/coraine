//
// FILE            authBase64.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "authBase64.h"                              // Own interface



// -----------------------------------------------------------------------------
//
// sextet - a base64url character's value, -1: not one (standard base64's + and / are taken too)
//
static int sextet(char c)
{
  if ((c >= 'A') && (c <= 'Z'))  return c - 'A';
  if ((c >= 'a') && (c <= 'z'))  return c - 'a' + 26;
  if ((c >= '0') && (c <= '9'))  return c - '0' + 52;
  if ((c == '-') || (c == '+'))  return 62;
  if ((c == '_') || (c == '/'))  return 63;

  return -1;
}



// -----------------------------------------------------------------------------
//
// authBase64UrlDecode -
//
int authBase64UrlDecode(const char* in, int len, unsigned char* out)
{
  while ((len > 0) && (in[len - 1] == '='))
    --len;

  if (len % 4 == 1)
    return -1;

  unsigned int  acc  = 0;
  int           bits = 0;
  int           n    = 0;

  for (int i = 0; i < len; i++)
  {
    int v = sextet(in[i]);

    if (v < 0)
      return -1;

    acc   = (acc << 6) | (unsigned int) v;
    bits += 6;

    if (bits >= 8)
    {
      bits    -= 8;
      out[n++] = (unsigned char) ((acc >> bits) & 0xFF);
    }
  }

  return n;
}
