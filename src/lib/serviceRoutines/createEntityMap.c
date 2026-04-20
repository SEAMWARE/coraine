//
// FILE            createEntityMap.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/entityMaps — § 5.14.4 / § 6.34.3.1 "Create EntityMap for
// Query Entities". Spec binds this to GET (counterintuitive, since 201
// Created is returned and the semantic is creation — see spec-doubts #14).
//
// URL parameters are the same as for GET /entities (§ 6.4.3.2). Instead
// of returning the matched entities, this route builds an EntityMap
// from the distributed-query result (with per-entity provenance from
// srcMap) and returns the map body.
//
// Implementation: force ?entityMap=true semantics + entityMapOnly=true
// and delegate to getEntities, which short-circuits with the map body
// and a 201 status once the map is built.
//

#include <stdbool.h>                                 // bool

#include "swNgsild/swNgsild.h"                       // swNgsild

#include "serviceRoutines/getEntities.h"             // getEntities
#include "serviceRoutines/createEntityMap.h"         // Own interface



// -----------------------------------------------------------------------------
//
// createEntityMap -
//
bool createEntityMap(void)
{
  swNgsild.entityMapCreate = true;
  swNgsild.entityMapOnly   = true;
  return getEntities();
}
