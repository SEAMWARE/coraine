//
// FILE            authFetch.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                   // FILE, fopen, fread, snprintf
#include <string.h>                                  // strncmp, memcpy

#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corBase/corUriPasswordMask.h"              // corUriPasswordMask
#include "corRest/CorRestVerb.h"                     // CorVerbGet
#include "corRest/corRestClient.h"                   // CorRestClientRequest, corRestClientSend

#include "authFetch.h"                               // Own interface



// -----------------------------------------------------------------------------
//
// fileRead -
//
static char* fileRead(const char* path, CorAlloc* kaP, char* errorBuf, int errorBufSize)
{
  FILE* fP = fopen(path, "r");

  if (fP == NULL)
  {
    snprintf(errorBuf, errorBufSize, "cannot open '%s'", path);
    return NULL;
  }

  fseek(fP, 0, SEEK_END);
  long size = ftell(fP);
  fseek(fP, 0, SEEK_SET);

  char* text = (size >= 0) ? (char*) corAlloc(kaP, size + 1) : NULL;

  if ((text == NULL) || (fread(text, 1, size, fP) != (size_t) size))
  {
    fclose(fP);
    snprintf(errorBuf, errorBufSize, "cannot read '%s'", path);
    return NULL;
  }

  text[size] = 0;
  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// authFetch -
//
char* authFetch(const char* where, CorAlloc* kaP, char* errorBuf, int errorBufSize)
{
  if (strncmp(where, "file://", 7) == 0)
    return fileRead(&where[7], kaP, errorBuf, errorBufSize);

  if ((strncmp(where, "http://", 7) != 0) && (strncmp(where, "https://", 8) != 0))
    return fileRead(where, kaP, errorBuf, errorBufSize);

  CorRestClientRequest   req;
  CorRestClientResponse  resp;

  memset(&resp, 0, sizeof(resp));
  corRestClientRequestInit(&req, CorVerbGet, where, kaP);
  corRestClientRequestHeader(&req, "Accept", "application/json");
  corRestClientRequestTimeout(&req, 5000, 10000);

  int   rc   = corRestClientSend(&req, &resp);
  char* text = NULL;
  char  shown[512];                                  // the URL as an error shows it: its password "***"

  corUriPasswordMask(where, shown, sizeof(shown));

  if (rc != 0)
    snprintf(errorBuf, errorBufSize, "GET %s: %s", shown, (resp.errorDetail[0] != 0) ? resp.errorDetail : "no response");
  else if ((resp.statusCode < 200) || (resp.statusCode > 299))
    snprintf(errorBuf, errorBufSize, "GET %s: %d", shown, resp.statusCode);
  else if (resp.body == NULL)
    snprintf(errorBuf, errorBufSize, "GET %s: no body", shown);
  else
  {
    text = (char*) corAlloc(kaP, resp.bodyLen + 1);

    if (text != NULL)
    {
      memcpy(text, resp.body, resp.bodyLen);
      text[resp.bodyLen] = 0;
    }
  }

  corRestClientResponseCleanup(&resp);
  corRestClientRequestCleanup(&req);
  return text;
}
