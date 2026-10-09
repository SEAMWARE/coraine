#ifndef BRIDGE_RECORDMAP_H_
#define BRIDGE_RECORDMAP_H_

//
// FILE            RecordMap.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The mapping of a RECORD CHANNEL - a Channel whose samples are records (a CKAN row, a CSV line, a
// JSON object with one member per column), each of which becomes one or more entities.
//
// A plain Channel binds an endpoint to ONE attribute of ONE entity, and a sample is that attribute's
// value. A record carries many values, about many entities, and which entity is named INSIDE it - a
// column holds the id. So a record Channel names no entity and no attribute; it names how to derive
// them from each record:
//
//   "records": { "<endpoint>": { "channelInfo": [ ... ],
//                                "entities": [
//     { "type": "BikeStation", "id": "urn:ngsi-ld:BikeStation:{Number}",
//       "attributes": {
//         "name":       { "column": "Name" },
//         "totalDocks": { "column": "Total_docks", "as": "number" },
//         "location":   { "point": { "longitude": "Longitude", "latitude": "Latitude" } },
//         "inDistrict": { "relationship": "urn:ngsi-ld:District:{District}" } } },
//     { "type": "District", "id": "urn:ngsi-ld:District:{District}",
//       "attributes": { "name": { "column": "District" } } } ] } }
//
// INBOUND ONLY. The broker writes what a record says; nothing is ever published back to it.
//
// Compiled in with COR_FEATURE_BRIDGE_RECORDS. The pointer in Channel is there either way, and NULL
// without the feature.
//



// -----------------------------------------------------------------------------
//
// RecordAs - what a column's value becomes
//
//   As        the value as the record has it: a string stays a string, a number a number
//   String    a number or a boolean rendered as text
//   Number    a string that holds a number becomes the number ("15" -> 15, "42.35" -> 42.35)
//   Boolean   "yes"/"true"/"y"/"1" -> true, "no"/"false"/"n"/"0" -> false (any case); a JSON
//             boolean as it is
//
// A value that cannot become what was asked for leaves the attribute out of that record's entity.
//
typedef enum RecordAs
{
  RecordAsIs      = 0,
  RecordAsString  = 1,
  RecordAsNumber  = 2,
  RecordAsBoolean = 3
} RecordAs;



// -----------------------------------------------------------------------------
//
// RecordAttrKind - how an attribute is made from a record
//
//   Column        a Property, the value of one column
//   Point         a GeoProperty Point, [longitude, latitude] from two columns
//   Relationship  a Relationship, its object a template over the columns
//
typedef enum RecordAttrKind
{
  RecordAttrColumn       = 0,
  RecordAttrPoint        = 1,
  RecordAttrRelationship = 2
} RecordAttrKind;



// -----------------------------------------------------------------------------
//
// RecordAttr - one attribute of a mapped entity
//
typedef struct RecordAttr
{
  char*               name;                           // EXPANDED, as a plain Channel's attribute
  RecordAttrKind      kind;
  char*               column;                         // Column: the column
  RecordAs            as;                             // Column: what its value becomes
  char*               longitude;                      // Point: the column holding the longitude
  char*               latitude;                       // Point: the column holding the latitude
  char*               object;                         // Relationship: a template, "urn:ngsi-ld:District:{District}"
  struct RecordAttr*  next;
} RecordAttr;



// -----------------------------------------------------------------------------
//
// RecordEntity - one entity made from each record
//
// The id is a template: "{Column}" is the column's value, each of its characters outside
// [A-Za-z0-9._-] replaced by one '_' ("South Boston" -> South_Boston). A record without a value in a column the id names
// makes no entity of this kind.
//
typedef struct RecordEntity
{
  char*                 type;                         // EXPANDED
  char*                 id;                           // a template
  RecordAttr*           attrs;
  struct RecordEntity*  next;
} RecordEntity;



// -----------------------------------------------------------------------------
//
// RecordMap - a record Channel's mapping
//
typedef struct RecordMap
{
  RecordEntity*  entities;
  char*          text;                                // the mapping as the file said it, for GET /channels
} RecordMap;

#endif  // BRIDGE_RECORDMAP_H_
