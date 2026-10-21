#ifndef BRIDGE_RECORDMAP_H_
#define BRIDGE_RECORDMAP_H_

//
// FILE            RecordMap.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The mapping of a DATACHANNEL (DataChannel.h) - how each of its records (a CKAN row, a CSV line, a
// JSON object with one member per column) becomes one or more entities. Which entity is named INSIDE
// the record - a column holds the id - so the mapping names how to derive them:
//
//   "dataChannels": { "<endpoint>": { "channelInfo": [ ... ],
//                                     "idEncoding": "merge",
//                                     "entities": [
//     { "type": "BikeStation", "id": "urn:ngsi-ld:BikeStation:{Number}",
//       "attributes": {
//         "name":       { "column": "Name" },
//         "totalDocks": { "column": "Total_docks", "as": "number" },
//         "location":   { "point": { "longitude": "Longitude", "latitude": "Latitude" } },
//         "inDistrict": { "relationship": "urn:ngsi-ld:District:{District}" } } },
//     { "type": "District", "id": "urn:ngsi-ld:District:{District}",
//       "attributes": { "name": { "column": "District" } } } ] } }
//
// More of the vocabulary - what an IoT Agent's measure needs (doc/iot-agent-measures.md § 2.4):
//
//   { "column": "t", "as": "integer" | "float" | "datetime" | "json", "unitCode": "CEL" }
//   { "geo": "loc" }             a GeoProperty from one column: "lat, lng", or GeoJSON (an object or its text)
//   { "json": "cfg" }            a JsonProperty
//   { "vocab": "state" }         a VocabProperty
//   { "languageMap": "label" }   a LanguageProperty, the column an object of { "<lang>": "<text>" }
//   { "value": <any JSON> }      a Property whose value is always that - a static attribute
//
//   an entity's "scope": "/garden/{Zone}"   - a template, the entity's scope
//   the entry's "otherColumns": "property"  - every column the mapping does not use becomes a Property of
//                                             its own name, on the first entity the record makes; "none"
//                                             (the default) drops them. A bridge's ngsild "otherColumns" is
//                                             the default for its DataChannels. A column whose name starts
//                                             with '@' is the bridge's (an IoT Agent's @deviceId) - never one
//
// The broker writes what a record says. Back to it goes only a client's write, on a DataChannel with
// channelDirection both (COR_FEATURE_CHANNEL_WRITE_BACK, dataChannelWriteBack.h).
//
// Compiled in with COR_FEATURE_DATA_CHANNELS.
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
//   Integer   a number, or a string holding one, that is whole ("15", 15.0 -> 15)
//   Float     a number, or a string holding one, as a floating point number ("15" -> 15.0)
//   DateTime  an ISO 8601 date-time, normalised to UTC (milliseconds, when it has them) - a DateTime value
//   Json      a string holding JSON, parsed; any other value as it is
//
// A value that cannot become what was asked for leaves the attribute out of that record's entity.
//
typedef enum RecordAs
{
  RecordAsIs      = 0,
  RecordAsString  = 1,
  RecordAsNumber  = 2,
  RecordAsBoolean = 3,
  RecordAsInteger = 4,
  RecordAsFloat   = 5,
  RecordAsDateTime = 6,
  RecordAsJson    = 7
} RecordAs;



// -----------------------------------------------------------------------------
//
// RecordAttrKind - how an attribute is made from a record
//
//   Column        a Property, the value of one column
//   Point         a GeoProperty Point, [longitude, latitude] from two columns
//   Relationship  a Relationship, its object a template over the columns
//   Geo           a GeoProperty from one column: "lat, lng" or GeoJSON
//   Json          a JsonProperty, the value of one column
//   Vocab         a VocabProperty, the value (a string, or an array of them) of one column
//   LanguageMap   a LanguageProperty, the column an object of language -> text
//   Constant      a Property whose value is the mapping's own - no column
//
typedef enum RecordAttrKind
{
  RecordAttrColumn       = 0,
  RecordAttrPoint        = 1,
  RecordAttrRelationship = 2,
  RecordAttrGeo          = 3,
  RecordAttrJson         = 4,
  RecordAttrVocab        = 5,
  RecordAttrLanguageMap  = 6,
  RecordAttrConstant     = 7
} RecordAttrKind;



// -----------------------------------------------------------------------------
//
// RecordAttr - one attribute of a mapped entity
//
typedef struct RecordAttr
{
  char*               name;                           // EXPANDED, as a plain Channel's attribute
  RecordAttrKind      kind;
  char*               column;                         // Column, Geo, Json, Vocab, LanguageMap: the column
  RecordAs            as;                             // Column: what its value becomes
  char*               longitude;                      // Point: the column holding the longitude
  char*               latitude;                       // Point: the column holding the latitude
  char*               object;                         // Relationship: a template, "urn:ngsi-ld:District:{District}"
  char*               constant;                       // Constant: the value as JSON text
  char*               unitCode;                       // the attribute's unitCode, NULL: none
  struct RecordAttr*  next;
} RecordAttr;



// -----------------------------------------------------------------------------
//
// RecordIdEncoding - how a column's value goes into an id (or a Relationship's object) template
//
//   Merge    each character outside [A-Za-z0-9._-] becomes one '_' ("South Boston" -> South_Boston) -
//            the default. Values that differ only there ("South Boston", "South_Boston", "South/Boston")
//            name ONE entity: their records are merged into it. Whether that is right, only the owner
//            of the data knows - and often it is
//   Percent  each byte outside [A-Za-z0-9._-] becomes %XX ("South Boston" -> South%20Boston): two
//            different values never name the same entity, and the value can be read back from the id
//
typedef enum RecordIdEncoding
{
  RecordIdMerge   = 0,
  RecordIdPercent = 1
} RecordIdEncoding;



// -----------------------------------------------------------------------------
//
// RecordEntity - one entity made from each record
//
// The id is a template: "{Column}" is the column's value, encoded as the map's idEncoding says. A record
// without a value in a column the id names makes no entity of this kind.
//
typedef struct RecordEntity
{
  char*                 type;                         // EXPANDED
  char*                 id;                           // a template
  char*                 scope;                        // a template, NULL: no scope
  RecordAttr*           attrs;
  struct RecordEntity*  next;
} RecordEntity;



// -----------------------------------------------------------------------------
//
// RecordMap - a DataChannel's mapping
//
// -----------------------------------------------------------------------------
//
// RecordOther - what becomes of a column the mapping does not use
//
typedef enum RecordOther
{
  RecordOtherUnset    = 0,                            // the entry said nothing: the bridge's default, else None
  RecordOtherNone     = 1,                            // dropped
  RecordOtherProperty = 2                             // a Property of its own name, on the first entity made
} RecordOther;



typedef struct RecordMap
{
  RecordEntity*     entities;
  RecordIdEncoding  idEncoding;
  RecordOther       otherColumns;
  char**            columnV;                          // every column the mapping uses - not "other"
  int               columns;
  char*             text;                             // "entities" as the file said it, for GET /dataChannels
} RecordMap;

#endif  // BRIDGE_RECORDMAP_H_
