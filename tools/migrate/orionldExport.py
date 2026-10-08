#!/usr/bin/env python3
#
# FILE            orionldExport.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# The READER half of a migration from an Orion-LD deployment to coraine (doc/migration.md):
# reads an Orion-LD database - the current state in MongoDB, the temporal history (TRoE) in
# PostgreSQL - and writes a coraine migration stream: one JSON record per line, expanded NGSI-LD,
# system timestamps and ids as they were. The WRITER half is coraine-import:
#
#   orionldExport.py --mongo mongodb://localhost:27017 --troe 'host=localhost user=postgres' > stream.ndjson
#   coraine-import -db corDB --dbDir /var/lib/coraine --troe timescale --file stream.ndjson
#
# Needs pymongo (current state) and psycopg 3 or psycopg2 (history), imported only when used.
#
import argparse
import datetime
import json
import re
import sys



# -----------------------------------------------------------------------------
#
# Time - Orion-LD keeps the current state's timestamps as epoch seconds (a double, millisecond
# fraction); the TRoE tables as SQL TIMESTAMP in UTC
#
def isoFromEpoch(seconds):
    if seconds is None:
        return None
    ms = int(round(float(seconds) * 1000))
    dt = datetime.datetime.fromtimestamp(ms // 1000, tz=datetime.timezone.utc)
    return dt.strftime('%Y-%m-%dT%H:%M:%S') + '.%03dZ' % (ms % 1000)


def isoFromSql(ts):
    if ts is None:
        return None
    if ts.tzinfo is not None:
        ts = ts.astimezone(datetime.timezone.utc).replace(tzinfo=None)
    return ts.strftime('%Y-%m-%dT%H:%M:%S') + '.%06dZ' % ts.microsecond



# -----------------------------------------------------------------------------
#
# Names - a key of a Mongo document cannot hold a '.', so Orion-LD writes '=' for each '.' of an
# expanded name in its keys (attrs, md, @datasets); the arrays of names keep the real dots
#
def dotted(eqName):
    return eqName.replace('=', '.')



# -----------------------------------------------------------------------------
#
# VALUE_KEY - the NGSI-LD member that holds the value of each attribute type
#
VALUE_KEY = {
    'Property':           'value',
    'GeoProperty':        'value',
    'Relationship':       'object',
    'LanguageProperty':   'languageMap',
    'VocabProperty':      'vocab',
    'JsonProperty':       'json',
    'ListProperty':       'valueList',
    'ListRelationship':   'objectList',
}



# -----------------------------------------------------------------------------
#
# attrType - an Orion-LD attribute type as NGSI-LD names it
#
def attrType(t):
    if t in (None, ''):
        return 'Property'
    if t == 'VocabularyProperty':   # an earlier name of the type
        return 'VocabProperty'
    return t



# -----------------------------------------------------------------------------
#
# subAttribute - one entry of an attribute's 'md' (a Sub-Attribute) as NGSI-LD
#
def subAttribute(m):
    t    = attrType(m.get('type'))
    out  = {'type': t}
    if 'value' in m:
        out[VALUE_KEY.get(t, 'value')] = m['value']
    for key in ('objectType', 'valueType', 'unitCode'):
        if key in m:
            out[key] = m[key]
    if 'observedAt' in m:
        out['observedAt'] = isoFromEpoch(m['observedAt']) if isinstance(m['observedAt'], (int, float)) else m['observedAt']
    if m.get('createdAt') is not None:
        out['createdAt'] = isoFromEpoch(m['createdAt'])
    if m.get('modifiedAt') is not None:
        out['modifiedAt'] = isoFromEpoch(m['modifiedAt'])
    for eqName, sub in (m.get('md') or {}).items():
        if isinstance(sub, dict):
            out[dotted(eqName)] = subAttribute(sub)
    return out



# -----------------------------------------------------------------------------
#
# attribute - an entry of the entity's 'attrs' (the default instance) as NGSI-LD
#
def attribute(a):
    t   = attrType(a.get('type'))
    out = {'type': t}

    if 'value' in a:
        out[VALUE_KEY.get(t, 'value')] = a['value']

    out['createdAt']  = isoFromEpoch(a.get('creDate'))
    out['modifiedAt'] = isoFromEpoch(a.get('modDate'))

    for eqName, m in (a.get('md') or {}).items():
        name = dotted(eqName)
        if name == 'observedAt':
            v = m.get('value') if isinstance(m, dict) else m
            out['observedAt'] = isoFromEpoch(v) if isinstance(v, (int, float)) else v
        elif name == 'unitCode':
            out['unitCode'] = m.get('value') if isinstance(m, dict) else m
        elif isinstance(m, dict):
            out[name] = subAttribute(m)

    return {k: v for k, v in out.items() if v is not None}



# -----------------------------------------------------------------------------
#
# datasetInstance - an instance with a datasetId (kept in the entity's '@datasets', in API shape)
#
def datasetInstance(d):
    t   = attrType(d.get('type'))
    out = {'type': t}
    for key, v in d.items():
        if key in ('type', 'createdAt', 'modifiedAt', 'creDate', 'modDate'):
            continue
        if key == 'value':
            out[VALUE_KEY.get(t, 'value')] = v
        elif key == 'observedAt' and isinstance(v, (int, float)):
            out['observedAt'] = isoFromEpoch(v)
        else:
            out[dotted(key)] = v
    created  = d.get('createdAt', d.get('creDate'))
    modified = d.get('modifiedAt', d.get('modDate'))
    if created is not None:
        out['createdAt']  = isoFromEpoch(created)
    if modified is not None:
        out['modifiedAt'] = isoFromEpoch(modified)
    return out



# -----------------------------------------------------------------------------
#
# entity - an Orion-LD entity document as an NGSI-LD Entity (normalized, expanded, sysAttrs)
#
def entity(doc):
    _id = doc['_id']
    out = {'id': _id['id'], 'type': _id['type']}

    if doc.get('creDate') is not None:
        out['createdAt'] = isoFromEpoch(doc['creDate'])
    if doc.get('modDate') is not None:
        out['modifiedAt'] = isoFromEpoch(doc['modDate'])

    for eqName, a in (doc.get('attrs') or {}).items():
        out[dotted(eqName)] = attribute(a)

    for eqName, instances in (doc.get('@datasets') or {}).items():
        name      = dotted(eqName)
        instances = instances if isinstance(instances, list) else [instances]
        converted = [datasetInstance(d) for d in instances if isinstance(d, dict)]
        if name in out:
            out[name] = [out[name]] + converted
        else:
            out[name] = converted if len(converted) > 1 else converted[0]

    return out



# -----------------------------------------------------------------------------
#
# qFromLdQ - Orion-LD's stored q (names '='-encoded, each followed by its '.value') as NGSI-LD q
#
# In an NGSI-LD q the '.' separates an attribute from its sub-attribute, so the dots of a full IRI
# travel %-encoded (%2E) - which is all the '=' of the stored names become.
#
LDQ_ATTR = re.compile(r'((?:https?|urn):[^;|()<>!~\s]*?)\.(?:value|object|languageMap)\b')

def qFromLdQ(ldQ):
    def repl(m):
        return m.group(1).replace('=', '%2E').replace('.', '%2E')
    return LDQ_ATTR.sub(repl, ldQ)



# -----------------------------------------------------------------------------
#
# geoQ - Orion-LD's stored geo-expression as an NGSI-LD geoQ
#
GEOMETRY = {'point': 'Point', 'polygon': 'Polygon', 'linestring': 'LineString', 'line': 'LineString',
            'multipoint': 'MultiPoint', 'multipolygon': 'MultiPolygon', 'multilinestring': 'MultiLineString'}

def coordinates(geometry, coords):
    if isinstance(coords, (list, tuple)):
        return coords
    try:
        return json.loads(coords)
    except (TypeError, ValueError):
        pass
    points = [[float(x) for x in p.split(',')] for p in coords.split(';') if p != '']
    if geometry == 'Point':
        return points[0]
    if geometry == 'Polygon':
        return [points]
    return points

def geoQ(expr):
    geometry = (expr or {}).get('geometry') or ''
    if geometry == '':
        return None
    g   = GEOMETRY.get(geometry.lower(), geometry)
    out = {'geometry': g, 'coordinates': coordinates(g, expr.get('coords')), 'georel': (expr.get('georel') or '').replace(':', '==')}
    if expr.get('geoproperty'):
        out['geoproperty'] = dotted(expr['geoproperty'])
    return out



# -----------------------------------------------------------------------------
#
# keyValues - an Orion-LD {k: v} object (headers, notifierInfo) as NGSI-LD's [{key, value}]
#
def keyValues(obj):
    if isinstance(obj, list):
        return obj
    return [{'key': k, 'value': v} for k, v in (obj or {}).items()]



# -----------------------------------------------------------------------------
#
# subscription - an Orion-LD csubs document as an NGSI-LD Subscription, with its counters
#
def subscription(doc):
    out = {'id': doc['_id'], 'type': 'Subscription'}

    if doc.get('name'):        out['subscriptionName'] = doc['name']
    if doc.get('description'): out['description']      = doc['description']

    entities = []
    for e in doc.get('entities') or []:
        sel = {}
        if str(e.get('isPattern', 'false')).lower() == 'true':
            if e.get('id') not in (None, '', '.*'):
                sel['idPattern'] = e['id']
        elif e.get('id'):
            sel['id'] = e['id']
        if e.get('type'):
            sel['type'] = e['type']
        entities.append(sel)
    if entities:
        out['entities'] = entities

    if doc.get('conditions'):
        out['watchedAttributes'] = doc['conditions']

    if doc.get('ldQ'):
        out['q'] = qFromLdQ(doc['ldQ'])

    g = geoQ(doc.get('expression'))
    if g is not None:
        out['geoQ'] = g

    for key in ('timeInterval', 'throttling'):
        if doc.get(key):
            out[key] = doc[key]

    if doc.get('expiration') and 0 < doc['expiration'] < 1e11:
        out['expiresAt'] = isoFromEpoch(doc['expiration'])

    if doc.get('status') in ('paused', 'inactive'):
        out['isActive'] = False

    for key in ('lang', 'showChanges', 'sysAttrs'):
        if key in doc:
            out[key] = doc[key]
    if doc.get('datasetId'):
        out['datasetId'] = doc['datasetId']
    if doc.get('ldContext'):
        out['jsonldContext'] = doc['ldContext']

    endpoint = {'uri': doc.get('reference')}
    if doc.get('mimeType'):     endpoint['accept']       = doc['mimeType']
    if doc.get('headers'):      endpoint['receiverInfo'] = keyValues(doc['headers'])
    if doc.get('notifierInfo'): endpoint['notifierInfo'] = keyValues(doc['notifierInfo'])

    notification = {'endpoint': endpoint, 'format': doc.get('format') or 'normalized'}
    if doc.get('attrs'):
        notification['attributes'] = doc['attrs']
    if doc.get('count'):       notification['timesSent']   = int(doc['count'])
    if doc.get('timesFailed'): notification['timesFailed'] = int(doc['timesFailed'])
    for key in ('lastNotification', 'lastSuccess', 'lastFailure'):
        if doc.get(key):
            notification[key] = isoFromEpoch(doc[key])
    out['notification'] = notification

    if doc.get('createdAt') is not None:  out['createdAt']  = isoFromEpoch(doc['createdAt'])
    if doc.get('modifiedAt') is not None: out['modifiedAt'] = isoFromEpoch(doc['modifiedAt'])

    return out



# -----------------------------------------------------------------------------
#
# interval - Orion-LD's {startAt, endAt} (epoch seconds) as NGSI-LD's (ISO 8601)
#
def interval(obj):
    return {k: isoFromEpoch(v) if isinstance(v, (int, float)) else v for k, v in obj.items()}



# -----------------------------------------------------------------------------
#
# registration - an Orion-LD registrations document as an NGSI-LD ContextSourceRegistration
#
REG_PASSTHROUGH = ('location', 'observationSpace', 'operationSpace', 'contextSourceInfo', 'tenant',
                   'scope', 'mode', 'operations', 'management', 'refreshRate')

def registration(doc):
    out = {'id': doc['_id'], 'type': 'ContextSourceRegistration'}

    if doc.get('name'):        out['registrationName'] = doc['name']
    if doc.get('description'): out['description']      = doc['description']

    information = []
    endpoint    = None
    for cr in doc.get('contextRegistration') or []:
        info = {}
        entities = []
        for e in cr.get('entities') or []:
            sel = {}
            if str(e.get('isPattern', 'false')).lower() == 'true':
                sel['idPattern'] = e['id']
            elif e.get('id'):
                sel['id'] = e['id']
            if e.get('type'):
                sel['type'] = e['type']
            entities.append(sel)
        if entities:
            info['entities'] = entities
        props = [a['name'] for a in cr.get('attrs') or [] if a.get('type') != 'Relationship']
        rels  = [a['name'] for a in cr.get('attrs') or [] if a.get('type') == 'Relationship']
        if props: info['propertyNames']     = props
        if rels:  info['relationshipNames'] = rels
        information.append(info)
        endpoint = endpoint or cr.get('providingApplication')

    out['information'] = information
    out['endpoint']    = endpoint

    for key in ('observationInterval', 'managementInterval'):
        if isinstance(doc.get(key), dict):
            out[key] = interval(doc[key])
    if doc.get('expiration') and 0 < doc['expiration'] < 1e11:
        out['expiresAt'] = isoFromEpoch(doc['expiration'])

    for key in REG_PASSTHROUGH:
        if key in doc:
            out[key] = doc[key]

    for eqName, v in (doc.get('properties') or {}).items():
        out[dotted(eqName)] = v

    if doc.get('createdAt') is not None:  out['createdAt']  = isoFromEpoch(doc['createdAt'])
    if doc.get('modifiedAt') is not None: out['modifiedAt'] = isoFromEpoch(doc['modifiedAt'])

    return out



# -----------------------------------------------------------------------------
#
# Stream - the migration stream (doc/migration.md)
#
class Stream:
    def __init__(self, out):
        self.out    = out
        self.counts = {}

    def write(self, kind, tenant, data):
        self.out.write(json.dumps({'kind': kind, 'tenant': tenant, 'data': data}, separators=(',', ':'), ensure_ascii=False, default=str) + '\n')
        self.counts[kind] = self.counts.get(kind, 0) + 1

    def header(self, source):
        self.out.write(json.dumps({'kind': 'header', 'format': 'coraine-migration', 'version': 1, 'source': source,
                                   'exportedAt': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}) + '\n')


def warn(text):
    sys.stderr.write('orionldExport: ' + text + '\n')



# -----------------------------------------------------------------------------
#
# tenantsOf - the tenants of a deployment: Orion-LD keeps one database per tenant,
# '<prefix>' for the default tenant and '<prefix>-<tenant>' for each other
#
def tenantsOf(client, prefix):
    tenants = {}
    for name in client.list_database_names():
        if name == prefix:
            tenants[''] = name
        elif name.startswith(prefix + '-'):
            tenants[name[len(prefix) + 1:]] = name
    return tenants



# -----------------------------------------------------------------------------
#
# exportCurrentState - registrations, subscriptions, entities - per tenant, in that order
#
# Registrations first: an exclusive or redirect registration refuses to be created over local
# entities that hold what it claims, and in the source the registration may well be older.
#
def exportCurrentState(stream, mongoUri, prefix, wanted, entityTypes):
    import pymongo

    client  = pymongo.MongoClient(mongoUri)
    tenants = tenantsOf(client, prefix)

    for tenant, dbName in sorted(tenants.items()):
        if wanted is not None and tenant not in wanted:
            continue
        db = client[dbName]

        for doc in db['registrations'].find():
            stream.write('registration', tenant, registration(doc))

        for doc in db['csubs'].find():
            if not isinstance(doc['_id'], str):
                warn('%s: subscription %s is an NGSIv2 subscription - not exported' % (dbName, doc['_id']))
                continue
            stream.write('subscription', tenant, subscription(doc))

        if 'casubs' in db.list_collection_names() and db['casubs'].estimated_document_count() > 0:
            warn('%s: the %d documents of casubs are not exported' % (dbName, db['casubs'].estimated_document_count()))

        for doc in db['entities'].find():
            e = entity(doc)
            entityTypes[(tenant, e['id'])] = e['type']
            stream.write('entity', tenant, e)

    return tenants



# -----------------------------------------------------------------------------
#
# TRoE - Orion-LD's history: one PostgreSQL database per tenant ('<prefix>', '<prefix>_<tenant>'),
# tables entities / attributes / subAttributes, one row per event, append-only
#
ENTITY_OP = {'Create': 'created', 'Replace': 'replaced', 'Delete': 'deleted'}
ATTR_OP   = {'Create': 'created', 'Append': 'created', 'Update': 'modified', 'Replace': 'replaced', 'Delete': 'deleted'}

GEO_COLUMNS = ('geoPoint', 'geoMultiPoint', 'geoPolygon', 'geoMultiPolygon', 'geoLineString', 'geoMultiLineString')


def pgConnect(dsn, dbName):
    try:
        import psycopg
        return psycopg.connect(dsn + ' dbname=' + dbName)
    except ImportError:
        import psycopg2
        return psycopg2.connect(dsn + ' dbname=' + dbName)


def pgDatabases(dsn):
    conn = pgConnect(dsn, 'postgres')
    cur  = conn.cursor()
    cur.execute('SELECT datname FROM pg_database')
    names = [r[0] for r in cur.fetchall()]
    conn.close()
    return names


def number(n):
    if n is None:
        return None
    return int(n) if float(n).is_integer() else float(n)


def rowValue(row):
    # the value of an attributes/subAttributes row, as (attribute type, value member, value)
    vt = row['valuetype']
    if vt == 'Relationship':
        return ('Relationship', 'object', row['text'] if row['text'] is not None else row['compound'])
    if vt == 'LanguageMap':
        return ('LanguageProperty', 'languageMap', row['compound'])
    if vt is not None and vt.startswith('Geo'):
        geo = next((row[c.lower()] for c in GEO_COLUMNS if row.get(c.lower()) is not None), None)
        return ('GeoProperty', 'value', json.loads(geo) if isinstance(geo, str) else geo)
    if vt == 'String':
        return ('Property', 'value', (row['text'] or '').replace('%27', "'"))
    if vt == 'Number':
        return ('Property', 'value', number(row['number']))
    if vt == 'Boolean':
        return ('Property', 'value', row['boolean'])
    if vt == 'DateTime':
        return ('Property', 'value', isoFromSql(row['datetime']))
    if vt == 'Compound':
        return ('Property', 'value', row['compound'])
    return ('Property', 'value', row['text'])


def pgRows(cur, sql):
    cur.execute(sql)
    names = [d[0].lower() for d in cur.description]
    for r in cur:
        yield dict(zip(names, r))


def pgColumns(cur, table):
    cur.execute("SELECT column_name FROM information_schema.columns WHERE table_name = %s", (table.lower(),))
    return {r[0].lower() for r in cur.fetchall()}


def geoSelect(columns):
    return ', '.join('ST_AsGeoJSON(ST_Force2D(%s::geometry)) AS %s' % (c, c) for c in GEO_COLUMNS if c.lower() in columns)


def exportHistory(stream, dsn, prefix, wanted, entityTypes):
    names   = pgDatabases(dsn)
    tenants = {}
    for name in names:
        if name == prefix:
            tenants[''] = name
        elif name.startswith(prefix + '_'):
            tenants[name[len(prefix) + 1:]] = name

    for tenant, dbName in sorted(tenants.items()):
        if wanted is not None and tenant not in wanted:
            continue
        conn = pgConnect(dsn, dbName)
        cur  = conn.cursor()

        types = {}
        for row in pgRows(cur, 'SELECT instanceId, ts, opMode, id, type FROM entities ORDER BY ts'):
            op = ENTITY_OP.get(row['opmode'])
            if op is None:
                warn('%s: entity %s: unknown opMode %s - skipped' % (dbName, row['id'], row['opmode']))
                continue
            data = {'id': row['id'], 'op': op, 'at': isoFromSql(row['ts'])}
            if row['type'] not in (None, 'NULL'):
                data['type'] = row['type']
                types[row['id']] = row['type']
            stream.write('temporalEntity', tenant, data)

        subs = {}
        sColumns = pgColumns(cur, 'subAttributes')
        sGeo     = geoSelect(sColumns)
        for row in pgRows(cur, 'SELECT instanceId, id, attrInstanceId, observedAt, unitCode, valueType, text, boolean, number, '
                               'datetime, compound%s, ts FROM subAttributes' % ((', ' + sGeo) if sGeo else '')):
            t, member, value = rowValue(row)
            sub = {'type': t, member: value, 'createdAt': isoFromSql(row['ts']), 'modifiedAt': isoFromSql(row['ts'])}
            if row['observedat'] is not None: sub['observedAt'] = isoFromSql(row['observedat'])
            if row['unitcode']   is not None: sub['unitCode']   = row['unitcode']
            subs.setdefault(row['attrinstanceid'], []).append((row['id'], sub))

        aColumns = pgColumns(cur, 'attributes')
        aGeo     = geoSelect(aColumns)
        for row in pgRows(cur, 'SELECT instanceId, id, opMode, entityId, observedAt, unitCode, datasetId, valueType, text, boolean, '
                               'number, datetime, compound%s, ts FROM attributes ORDER BY ts' % ((', ' + aGeo) if aGeo else '')):
            op = ATTR_OP.get(row['opmode'])
            if op is None:
                warn('%s: %s %s: unknown opMode %s - skipped' % (dbName, row['entityid'], row['id'], row['opmode']))
                continue

            etype = types.get(row['entityid']) or entityTypes.get((tenant, row['entityid']))
            if etype is None:
                warn('%s: %s: no entity type known (no entities row, no current state) - its history is skipped' % (dbName, row['entityid']))
                continue

            ts       = isoFromSql(row['ts'])
            instance = {'instanceId': row['instanceid'], 'createdAt': ts, 'modifiedAt': ts}
            if row['datasetid'] not in (None, 'None'):
                instance['datasetId'] = row['datasetid']

            if op != 'deleted':
                t, member, value = rowValue(row)
                instance['type'] = t
                instance[member] = value
                if row['observedat'] is not None: instance['observedAt'] = isoFromSql(row['observedat'])
                if row['unitcode']   is not None: instance['unitCode']   = row['unitcode']
                for name, sub in subs.get(row['instanceid'], []):
                    instance[name] = sub

            stream.write('temporalInstance', tenant, {'id': row['entityid'], 'type': etype, 'attr': row['id'], 'op': op, 'instance': instance})

        conn.close()



# -----------------------------------------------------------------------------
#
# main
#
def main():
    ap = argparse.ArgumentParser(description='Read an Orion-LD database (current state in MongoDB, history in PostgreSQL) '
                                             'and write a coraine migration stream (doc/migration.md) - import it with coraine-import')
    ap.add_argument('--mongo',     default='mongodb://localhost:27017', help='MongoDB URI of the current state (default: %(default)s)')
    ap.add_argument('--db',        default='orion', help='the database name prefix the deployment ran with (-db; default: %(default)s)')
    ap.add_argument('--troe',      default=None, help="libpq connection string of the TRoE server, without dbname (e.g. 'host=localhost user=postgres password=...'); none: no history")
    ap.add_argument('--troeDb',    default=None, help='the TRoE database name prefix (default: the --db prefix)')
    ap.add_argument('--tenants',   default=None, help="comma-separated tenants to export ('' or 'default' for the default tenant; default: all)")
    ap.add_argument('--noCurrent', action='store_true', help='leave the current state out')
    ap.add_argument('--out',       default='-', help='the stream file (default: stdout)')
    args = ap.parse_args()

    wanted = None
    if args.tenants is not None:
        wanted = {('' if t in ('', 'default') else t) for t in args.tenants.split(',')}

    out    = sys.stdout if args.out == '-' else open(args.out, 'w', encoding='utf-8')
    stream = Stream(out)
    stream.header('orion-ld')

    entityTypes = {}
    if not args.noCurrent:
        exportCurrentState(stream, args.mongo, args.db, wanted, entityTypes)
    if args.troe is not None:
        exportHistory(stream, args.troe, args.troeDb or args.db, wanted, entityTypes)

    if out is not sys.stdout:
        out.close()

    sys.stderr.write('orionldExport: ' + ', '.join('%d %s' % (n, k) for k, n in sorted(stream.counts.items())) + '\n')


if __name__ == '__main__':
    main()
