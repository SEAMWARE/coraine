#ifndef AUTH_BASE64_H_
#define AUTH_BASE64_H_

//
// FILE            authBase64.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// authBase64UrlDecode - base64url (RFC 4648 § 5), padding optional, into out (room for len * 3 / 4 + 3
// bytes); the number of bytes decoded, -1: not base64url
//
extern int authBase64UrlDecode(const char* in, int len, unsigned char* out);

#endif  // AUTH_BASE64_H_
