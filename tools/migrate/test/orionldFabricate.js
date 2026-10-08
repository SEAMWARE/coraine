//
// FILE            orionldFabricate.js
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// mongosh script: a small, made-up Orion-LD current state - the databases 'migtestorion' (default
// tenant) and 'migtestorion-t1' (tenant t1) - laid out the way an Orion-LD deployment keeps it, for
// orionldE2E.sh. Every kind of attribute the reader converts, a multi-instance attribute, Sub-
// Attributes, a dotted name, two subscriptions with counters, a registration.
//
// The prefix is 'migtestorion' and nothing else is touched: both databases are dropped first.
//

const V    = 'https://uri.etsi.org/ngsi-ld/default-context/';
const VEQ  = 'https://uri=etsi=org/ngsi-ld/default-context/';
const CORE = 'https://uri.etsi.org/ngsi-ld/v1/ngsi-ld-core-context-v1.9.jsonld';

const t0 = 1704164645.123;   // 2024-01-02T03:04:05.123Z
const t1 = 1706843045.456;   // 2024-02-02T03:04:05.456Z
const t2 = 1704412800.0;     // 2024-01-05T00:00:00.000Z
const t3 = 1704499200.0;     // 2024-01-06T00:00:00.000Z

const dflt = db.getSiblingDB('migtestorion');
const t1db = db.getSiblingDB('migtestorion-t1');

dflt.dropDatabase();
t1db.dropDatabase();


//
// Entities
//
dflt.entities.insertMany([
  {
    _id: { id: 'urn:ngsi-ld:Vehicle:V1', type: V + 'Vehicle', servicePath: '/' },
    attrNames: [V + 'speed', 'location', V + 'isParked', V + 'name', V + 'category', V + 'config', V + 'temperature'],
    attrs: {
      [VEQ + 'speed']: {
        type: 'Property', creDate: t0, modDate: t1, value: 42,
        mdNames: ['observedAt', 'unitCode', V + 'source'],
        md: {
          observedAt: { value: 1706843000.0 },
          unitCode:   { value: 'KMH' },
          [VEQ + 'source']: { type: 'Property', value: 'radar', createdAt: t2, modifiedAt: t3 }
        }
      },
      location:             { type: 'GeoProperty',      creDate: t0, modDate: t0, value: { type: 'Point', coordinates: [13.4, 52.5] }, mdNames: [] },
      [VEQ + 'isParked']:   { type: 'Relationship',     creDate: t0, modDate: t0, value: 'urn:ngsi-ld:Parking:P1', mdNames: [] },
      [VEQ + 'name']:       { type: 'LanguageProperty', creDate: t0, modDate: t0, value: { en: 'car', es: 'coche' }, mdNames: [] },
      [VEQ + 'category']:   { type: 'VocabProperty',    creDate: t0, modDate: t0, value: V + 'sedan', mdNames: [] },
      [VEQ + 'config']:     { type: 'Property',         creDate: t0, modDate: t0, value: { mode: 'eco', levels: [1, 2] }, mdNames: [] },
      [VEQ + 'temperature']:{ type: 'Property',         creDate: t0, modDate: t0, value: 20, mdNames: [] }
    },
    '@datasets': {
      [VEQ + 'temperature']: [
        { type: 'Property', value: 21, datasetId: 'urn:ngsi-ld:dataset:engine', createdAt: t2, modifiedAt: t3 },
        { type: 'Property', value: 22, datasetId: 'urn:ngsi-ld:dataset:cabin',  createdAt: t2, modifiedAt: t2 }
      ]
    },
    creDate: t0, modDate: t1, lastCorrelator: ''
  },
  {
    _id: { id: 'urn:ngsi-ld:Parking:P1', type: 'https://example.org/Parking', servicePath: '/' },
    attrNames: ['https://example.org/v1.0/capacity'],
    attrs: {
      'https://example=org/v1=0/capacity': { type: 'Property', creDate: t2, modDate: t3, value: 100, mdNames: [] }
    },
    creDate: t2, modDate: t3, lastCorrelator: ''
  }
]);

t1db.entities.insertOne({
  _id: { id: 'urn:ngsi-ld:Room:R1', type: 'https://example.org/Room', servicePath: '/' },
  attrNames: ['https://example.org/temperature'],
  attrs: { 'https://example=org/temperature': { type: 'Property', creDate: t2, modDate: t2, value: 21.5, mdNames: [] } },
  creDate: t2, modDate: t2, lastCorrelator: ''
});


//
// Subscriptions
//
dflt.csubs.insertMany([
  {
    _id: 'urn:ngsi-ld:Subscription:S1', name: 'S1', description: 'speeding',
    entities: [{ id: 'urn:ngsi-ld:Vehicle:V1', type: V + 'Vehicle', isPattern: 'false', isTypePattern: false }],
    conditions: [V + 'speed'],
    ldQ: VEQ + 'speed.value>40',
    expression: { geometry: '', coords: '', georel: '', geoproperty: '', q: 'P;!P', mq: 'P.P;!P.P' },
    attrs: [V + 'speed'],
    reference: 'http://localhost:9997/notify', mimeType: 'application/json', format: 'normalized',
    throttling: 0, status: 'active', custom: false, servicePath: '/#', blacklist: false,
    ldContext: CORE,
    createdAt: 1704067200.5, modifiedAt: 1705000000.25,
    count: 5, timesFailed: 1, lastNotification: 1706000000.0, lastSuccess: 1705990000.0, lastFailure: 1706000000.0
  },
  {
    _id: 'urn:ngsi-ld:Subscription:S2',
    entities: [{ id: '.*', type: V + 'Vehicle', isPattern: 'true', isTypePattern: false }],
    expression: { geometry: 'point', coords: '13.4,52.5', georel: 'near;maxDistance:2000', geoproperty: 'location', q: '', mq: '' },
    reference: 'http://localhost:9997/notify2', mimeType: 'application/json', format: 'keyValues',
    headers: { 'X-Auth-Token': 'abc' },
    expiration: 1893456000.0,
    throttling: 0, status: 'paused', custom: false, servicePath: '/#', blacklist: false,
    ldContext: CORE,
    createdAt: 1704067200.0, modifiedAt: 1704067200.0
  }
]);

t1db.csubs.insertOne({
  _id: 'urn:ngsi-ld:Subscription:S3',
  entities: [{ id: '.*', type: 'https://example.org/Room', isPattern: 'true', isTypePattern: false }],
  reference: 'http://localhost:9997/notify3', mimeType: 'application/json', format: 'normalized',
  throttling: 0, status: 'active', custom: false, servicePath: '/#', blacklist: false,
  ldContext: CORE,
  createdAt: 1704412800.0, modifiedAt: 1704412800.0
});


//
// Registrations
//
dflt.registrations.insertOne({
  _id: 'urn:ngsi-ld:ContextSourceRegistration:R1', name: 'R1', description: 'remote vehicles',
  contextRegistration: [{
    entities: [{ id: 'urn:ngsi-ld:Vehicle:A.*', type: V + 'Vehicle', isPattern: 'true' }],
    attrs: [{ name: V + 'speed', type: 'Property' }, { name: V + 'isParked', type: 'Relationship' }],
    providingApplication: 'http://my.csource.org:1026'
  }],
  observationInterval: { startAt: 1546250400.123, endAt: 1861869600.456 },
  expiration: 1861869601.234,
  contextSourceInfo: [{ key: 'H1', value: 'H1 value' }],
  scope: ['/s1', '/s2'],
  mode: 'inclusive',
  operations: ['retrieveEntity', 'queryEntity'],
  management: { localOnly: true, timeout: 5000, cooldown: 60000 },
  properties: { [VEQ + 'P1']: 1 },
  status: 'active',
  createdAt: 1704067200.0, modifiedAt: 1704153600.0
});

print('fabricated: migtestorion (' + dflt.entities.countDocuments() + ' entities, ' + dflt.csubs.countDocuments() + ' subscriptions, ' +
      dflt.registrations.countDocuments() + ' registrations), migtestorion-t1 (' + t1db.entities.countDocuments() + ' entities, ' +
      t1db.csubs.countDocuments() + ' subscriptions)');
