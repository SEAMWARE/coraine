#ifndef STARTUP_STARTUPDEFAULTUSERCONTEXT_H_
#define STARTUP_STARTUPDEFAULTUSERCONTEXT_H_

//
// FILE            startupDefaultUserContext.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// startupDefaultUserContext - the default user @context (--defaultUserContext/-duc): a URL, or a FILE
//
// A URL is used as it is. A file - a path, or file://<path>, as Orion-LD's -duc takes it and as the DDS
// mapping tool writes it (dds-context.jsonld) - is read once, its "@context" hosted by the broker as the
// @context 'defaultUserContext', and the default user @context is then its URL:
// <endpoint>/ngsi-ld/v1/jsonldContexts/defaultUserContext - what a client is given in a Link header, and can
// fetch (with COR_FEATURE_CONTEXT_HOSTING).
//
// After startupCoreContext (the @context cache) and with the broker's HTTP endpoint known.
//
// @return the URL to use as the default user @context; NULL with *errorP saying why (a file that cannot be
//         read, is not JSON, has no "@context")
//
extern const char* startupDefaultUserContext(const char* duc, const char* endpoint, const char** errorP);

#endif  // STARTUP_STARTUPDEFAULTUSERCONTEXT_H_
