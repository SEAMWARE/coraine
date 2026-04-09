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
// mongocGeoIndexEnsure - scan a newly created entity for GeoProperty attributes and create indexes
//
extern void mongocGeoIndexEnsure(Tenant* tenantP, KjNode* entityP, mongoc_collection_t* collP);

#endif  // MONGOC_MONGOCGEOINDEX_H_
