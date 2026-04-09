//
// FILE            mongocDotEscape.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stdbool.h>                                     // bool
#include <string.h>                                      // strchr

#include "kalloc/kaStrdup.h"                             // kaStrdup

#include "currentState/mongoc/mongocDotEscape.h"         // Own interface



// -----------------------------------------------------------------------------
//
// mongocEscapeDotsInKey - replace '.' with fullwidth period U+FF0E (3 bytes: EF BC 8E)
//
// Returns the original key if no dots, or a thread-local static buffer.
//
const char* mongocEscapeDotsInKey(const char* key)
{
  if (key == NULL || strchr(key, '.') == NULL)
    return key;

  static __thread char buf[1024];
  char*       out    = buf;
  char*       outEnd = buf + sizeof(buf) - 4;

  for (const char* p = key; *p != '\0' && out < outEnd; ++p)
  {
    if (*p == '.')
    {
      *out++ = (char) 0xEF;
      *out++ = (char) 0xBC;
      *out++ = (char) 0x8E;
    }
    else
    {
      *out++ = *p;
    }
  }

  *out = '\0';
  return buf;
}



// -----------------------------------------------------------------------------
//
// mongocUnescapeDotsInKey - replace fullwidth period U+FF0E back to '.'
//
// Returns the original key if no fullwidth periods, or a kaP-allocated copy.
//
const char* mongocUnescapeDotsInKey(KAlloc* kaP, const char* key)
{
  // Quick scan for the 0xEF byte that starts U+FF0E
  bool hasEscaped = false;

  for (const char* p = key; *p != '\0'; ++p)
  {
    if ((unsigned char) p[0] == 0xEF &&
        (unsigned char) p[1] == 0xBC &&
        (unsigned char) p[2] == 0x8E)
    {
      hasEscaped = true;
      break;
    }
  }

  if (!hasEscaped)
    return key;

  char* buf = kaStrdup(kaP, key);
  char* out = buf;

  for (const char* p = key; *p != '\0'; )
  {
    if ((unsigned char) p[0] == 0xEF &&
        (unsigned char) p[1] == 0xBC &&
        (unsigned char) p[2] == 0x8E)
    {
      *out++ = '.';
      p += 3;
    }
    else
    {
      *out++ = *p++;
    }
  }

  *out = '\0';
  return buf;
}
