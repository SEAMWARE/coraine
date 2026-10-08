--
-- FILE            orionldFabricate.sql
--
-- AUTHOR          Ken Zangelin
--
-- Copyright 2026 Seamware
-- SPDX-License-Identifier: Apache-2.0
--
-- A small, made-up Orion-LD temporal history (TRoE) for orionldE2E.sh: run with psql against an
-- EMPTY database ('migtestorion' for the default tenant, 'migtestorion_t1' for tenant t1 - the
-- script creates both), shaped the way an Orion-LD deployment keeps it: tables entities,
-- attributes, subAttributes, one row per event, the name of an expanded attribute with its dots,
-- the default instance's datasetId the literal 'None', an entity deletion's type the literal 'NULL'.
--
-- Run as: psql -v tenant=default -f orionldFabricate.sql   (or -v tenant=t1)
--

CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TYPE OperationMode AS ENUM ('Create', 'Append', 'Update', 'Replace', 'Delete');
CREATE TYPE ValueType     AS ENUM ('String', 'Number', 'Boolean', 'Relationship', 'Compound', 'DateTime',
                                   'GeoPoint', 'GeoMultiPoint', 'GeoPolygon', 'GeoMultiPolygon',
                                   'GeoLineString', 'GeoMultiLineString', 'LanguageMap');

CREATE TABLE entities (
  instanceId  TEXT          NOT NULL,
  ts          TIMESTAMP     NOT NULL,
  opMode      OperationMode,
  id          TEXT          NOT NULL,
  type        TEXT          NOT NULL,
  PRIMARY KEY (instanceId, ts)
);

CREATE TABLE attributes (
  instanceId          TEXT     NOT NULL,
  id                  TEXT     NOT NULL,
  opMode              OperationMode,
  entityId            TEXT     NOT NULL,
  observedAt          TIMESTAMP,
  subProperties       BOOL,
  unitCode            TEXT,
  datasetId           VARCHAR  NOT NULL,
  valueType           ValueType,
  text                TEXT,
  boolean             BOOL,
  number              FLOAT8,
  datetime            TIMESTAMP,
  compound            JSONB,
  geoPoint            GEOGRAPHY(POINTZ, 4326),
  geoMultiPoint       GEOGRAPHY(MULTIPOINTZ, 4326),
  geoPolygon          GEOGRAPHY(POLYGONZ, 4326),
  geoMultiPolygon     GEOGRAPHY(MULTIPOLYGONZ, 4326),
  geoLineString       GEOGRAPHY(LINESTRINGZ, 4326),
  geoMultiLineString  GEOGRAPHY(MULTILINESTRINGZ, 4326),
  ts                  TIMESTAMP NOT NULL,
  PRIMARY KEY (instanceId, datasetId, ts)
);

CREATE TABLE subAttributes (
  instanceId          TEXT     NOT NULL,
  id                  TEXT     NOT NULL,
  entityId            TEXT     NOT NULL,
  attrInstanceId      TEXT     NOT NULL,
  attrDatasetId       VARCHAR  NOT NULL,
  observedAt          TIMESTAMP,
  unitCode            TEXT,
  valueType           ValueType,
  text                TEXT,
  boolean             BOOL,
  number              FLOAT8,
  datetime            TIMESTAMP,
  compound            JSONB,
  geoPoint            GEOGRAPHY(POINTZ, 4326),
  geoMultiPoint       GEOGRAPHY(MULTIPOINTZ, 4326),
  geoPolygon          GEOGRAPHY(POLYGONZ, 4326),
  geoMultiPolygon     GEOGRAPHY(MULTIPOLYGONZ, 4326),
  geoLineString       GEOGRAPHY(LINESTRINGZ, 4326),
  geoMultiLineString  GEOGRAPHY(MULTILINESTRINGZ, 4326),
  ts                  TIMESTAMP NOT NULL,
  PRIMARY KEY (instanceId, ts)
);

\if :{?tenant}
\else
\set tenant default
\endif

SELECT :'tenant' = 'default' AS isdefault \gset

\if :isdefault

-- V1: created, speed updated; a sub-attribute; a GeoProperty; a Relationship; a datasetId instance
-- V9: created and deleted, with an attribute deleted before it - its history outlives it
INSERT INTO entities VALUES
  ('urn:ngsi-ld:attribute:instance:e-0001', '2024-01-02 03:04:05.123', 'Create', 'urn:ngsi-ld:Vehicle:V1', 'https://uri.etsi.org/ngsi-ld/default-context/Vehicle'),
  ('urn:ngsi-ld:attribute:instance:e-0002', '2024-01-02 03:04:06.000', 'Create', 'urn:ngsi-ld:Vehicle:V9', 'https://uri.etsi.org/ngsi-ld/default-context/Vehicle'),
  ('urn:ngsi-ld:attribute:instance:e-0003', '2024-02-02 03:04:05.456', 'Delete', 'urn:ngsi-ld:Vehicle:V9', 'NULL');

INSERT INTO attributes (instanceId, id, opMode, entityId, observedAt, subProperties, unitCode, datasetId, valueType, text, number, ts) VALUES
  ('urn:ngsi-ld:attribute:instance:a-0001', 'https://uri.etsi.org/ngsi-ld/default-context/speed',       'Create', 'urn:ngsi-ld:Vehicle:V1', '2024-01-02 03:00:00', true,  'KMH', 'None',                       'Number',       NULL,                     40, '2024-01-02 03:04:05.123'),
  ('urn:ngsi-ld:attribute:instance:a-0002', 'https://uri.etsi.org/ngsi-ld/default-context/speed',       'Update', 'urn:ngsi-ld:Vehicle:V1', '2024-02-02 03:03:20', false, 'KMH', 'None',                       'Number',       NULL,                     42, '2024-02-02 03:04:05.456'),
  ('urn:ngsi-ld:attribute:instance:a-0004', 'https://uri.etsi.org/ngsi-ld/default-context/isParked',    'Create', 'urn:ngsi-ld:Vehicle:V1', NULL,                  false, NULL,  'None',                       'Relationship', 'urn:ngsi-ld:Parking:P1', NULL, '2024-01-02 03:04:05.123'),
  ('urn:ngsi-ld:attribute:instance:a-0005', 'https://uri.etsi.org/ngsi-ld/default-context/temperature', 'Create', 'urn:ngsi-ld:Vehicle:V1', NULL,                  false, NULL,  'urn:ngsi-ld:dataset:engine', 'Number',       NULL,                     21, '2024-01-05 00:00:00'),
  ('urn:ngsi-ld:attribute:instance:a-0006', 'https://uri.etsi.org/ngsi-ld/default-context/name',        'Create', 'urn:ngsi-ld:Vehicle:V9', NULL,                  false, NULL,  'None',                       'String',       'it%27s gone',            NULL, '2024-01-02 03:04:06.000');

INSERT INTO attributes (instanceId, id, opMode, entityId, subProperties, datasetId, valueType, geoPoint, ts) VALUES
  ('urn:ngsi-ld:attribute:instance:a-0003', 'location', 'Create', 'urn:ngsi-ld:Vehicle:V1', false, 'None', 'GeoPoint', ST_GeogFromText('SRID=4326;POINT Z(13.4 52.5 0)'), '2024-01-02 03:04:05.123');

INSERT INTO attributes (instanceId, id, opMode, entityId, datasetId, ts) VALUES
  ('urn:ngsi-ld:attribute:instance:a-0007', 'https://uri.etsi.org/ngsi-ld/default-context/name', 'Delete', 'urn:ngsi-ld:Vehicle:V9', 'None', '2024-01-20 00:00:00');

INSERT INTO subAttributes (instanceId, id, entityId, attrInstanceId, attrDatasetId, valueType, text, ts) VALUES
  ('urn:ngsi-ld:attribute:instance:s-0001', 'https://uri.etsi.org/ngsi-ld/default-context/source', 'urn:ngsi-ld:Vehicle:V1', 'urn:ngsi-ld:attribute:instance:a-0001', 'None', 'String', 'radar', '2024-01-02 03:04:05.123');

\else

INSERT INTO entities VALUES
  ('urn:ngsi-ld:attribute:instance:e-0101', '2024-01-05 00:00:00', 'Create', 'urn:ngsi-ld:Room:R1', 'https://example.org/Room');

INSERT INTO attributes (instanceId, id, opMode, entityId, subProperties, datasetId, valueType, number, ts) VALUES
  ('urn:ngsi-ld:attribute:instance:a-0101', 'https://example.org/temperature', 'Create', 'urn:ngsi-ld:Room:R1', false, 'None', 'Number', 21.5, '2024-01-05 00:00:00');

\endif
