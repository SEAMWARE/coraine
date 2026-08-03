#ifndef MONGOC_MONGOCGEOINDEX_H_
#define MONGOC_MONGOCGEOINDEX_H_

//
// FILE            mongocGeoIndex.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <mongoc/mongoc.h>                           // mongoc_collection_t

#include "kjson/KjNode.h"                            // KjNode
#include "db/Tenant.h"                               // Tenant



// -----------------------------------------------------------------------------
//
// mongocGeoIndexInit - scan existing entities for GeoProperty attributes and create 2dsphere indexes
//
extern void mongocGeoIndexInit(Tenant* tenantP, mongoc_collection_t* collP);



// -----------------------------------------------------------------------------
//
// mongocGeoIndexEnsure - create the 2dsphere indexes this entity's GeoProperties need
//
// Call BEFORE the write. Returns NULL when every index is in place, or the (long)
// name of the Attribute whose index could not be built — which happens only when
// an existing Entity already holds a non-geo value under that name. Refuse the
// write with DB_GEO_TYPE_CONFLICT; nothing has been stored yet.
//
// Already-indexed attributes cost one cache lookup, so the ordinary write is
// unaffected: only the first appearance of a name as a GeoProperty in a tenant
// creates an index at all.
//
extern const char* mongocGeoIndexEnsure(Tenant* tenantP, KjNode* entityP, mongoc_collection_t* collP);



// -----------------------------------------------------------------------------
//
// mongocGeoIndexMixedName - after a "Can't extract geo keys" rejection, name the
//                           Attribute that is geo-indexed but written as another type
//
// Returns NULL when the rejection was really about a bad geometry rather than a
// clash of Attribute kinds.
//
extern const char* mongocGeoIndexMixedName(Tenant* tenantP, KjNode* entityP);

#endif  // MONGOC_MONGOCGEOINDEX_H_
