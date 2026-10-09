#!/usr/bin/env python3
#
# FILE            soak.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# soak.py - mixed load on ONE broker for a long time, at a moderate rate: does it stay up, stay within
# its memory, keep its log clean, deliver every notification and keep its data right?
#
#   test/soak/soak.py --db mongoc|corDB [--minutes 120] [--rate 40] [--workers 4] [--port 19210]
#                     [--receiverPort 19211] [--broker coraine] [--work /tmp/soak]
#
# A stability check, not a measurement: the rate is fixed and low, and no throughput or latency is
# compared with anything. The broker is a RELEASE build started by this script, in the foreground,
# its log in <work>/broker.log:
#
#   mongoc  --database mongoc --dbHost $COR_MONGO_HOST --dbName soak<epoch> --troe none
#   corDB   --database corDB --dbDir <work>/cordb --troe none          (the durable configuration)
#
# The load - each worker owns its own entities (urn:ngsi-ld:SoakA:w<k>-<n>), so what it expects is
# known exactly:
#
#   create, PATCH (speed), retrieve (speed as last written), query (type + q, count=true), batch
#   create / update / delete, delete, a subscription created and deleted, GET /types
#
# with at most --maxEntities live entities per worker (a worker deletes instead of creating at the
# bound), so the store reaches a steady size and memory has no reason to grow.
#
# Three subscriptions on SoakA, notified to the receiver this script runs:
#
#   /n/created  entityCreated        one notified entity per entity created (single and batch)
#   /n/updated  attributeUpdated on speed - one per PATCH / batch-updated entity
#   /n/deleted  entityDeleted        one per entity deleted (single and batch)
#
# Verdict - each one a FAIL line and a non-zero exit:
#
#   alive       the broker never exited and answered every request (no transport error)
#   statuses    every request got the status it should (201, 204, 200)
#   reads       a retrieve returned the speed last written
#   memory      RssAnon (the heap: a mapped store file is RssFile and not counted) after the warm-up
#               (the first 15 %, at least 5 minutes) grew by at most max(--rssMaxGrowthPct %,
#               --rssMaxGrowthMiB) by the end - medians of five samples at either end
#   log         no E or X line in the broker's log (a request refused with a 4xx is not an error)
#   notified    the receiver's count per subscription equals the count expected, once delivery is
#               given up to a minute to drain
#   consistent  at the end: the ids of every live entity, each one's speed, the entity count
#               (NGSILD-Results-Count) and the three subscriptions, read back and compared
#
# Output: one line per check, the RSS samples in <work>/rss.csv, and a markdown summary in
# <work>/summary.md. Exit code: the number of failed checks.
#
import argparse
import http.client
import http.server
import json
import os
import random
import re
import statistics
import subprocess
import sys
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument('--db', required=True, choices=['mongoc', 'corDB'])
ap.add_argument('--minutes', type=float, default=120)
ap.add_argument('--rate', type=float, default=40, help='requests per second, all workers together')
ap.add_argument('--workers', type=int, default=4)
ap.add_argument('--port', type=int, default=19210)
ap.add_argument('--receiverPort', type=int, default=19211)
ap.add_argument('--broker', default='coraine')
ap.add_argument('--work', default='/tmp/soak')
ap.add_argument('--maxEntities', type=int, default=500, help='live entities per worker at most')
ap.add_argument('--rssMaxGrowthPct', type=float, default=25)
ap.add_argument('--rssMaxGrowthMiB', type=float, default=48)
ap.add_argument('--sampleSeconds', type=float, default=30)
args = ap.parse_args()

os.makedirs(args.work, exist_ok=True)
T = 'SoakA'
FAILS = []
lock = threading.Lock()


def log(msg):
    print(time.strftime('%H:%M:%S ') + msg, flush=True)


def fail(check, msg):
    with lock:
        FAILS.append((check, msg))
    log(f'FAIL  {check}: {msg}')


#
# The receiver - entities notified, per subscription path
#
notified = {'created': 0, 'updated': 0, 'deleted': 0, 'other': 0}


class Receiver(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        key = self.path.rsplit('/', 1)[-1]
        try:
            n = len(json.loads(body).get('data', []))
        except Exception:
            n = 0
        with lock:
            notified[key if key in notified else 'other'] += n
        self.send_response(200)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, *a):
        pass


receiver = http.server.ThreadingHTTPServer(('127.0.0.1', args.receiverPort), Receiver)
threading.Thread(target=receiver.serve_forever, daemon=True).start()

#
# The broker
#
if args.db == 'mongoc':
    storeArgs = ['--database', 'mongoc', '--dbHost', os.environ.get('COR_MONGO_HOST', 'localhost'),
                 '--dbName', f'soak{int(time.time())}', '--troe', 'none']
else:
    storeArgs = ['--database', 'corDB', '--dbDir', os.path.join(args.work, 'cordb'), '--troe', 'none']
cmd = [args.broker, '--foreground', '--port', str(args.port), '--httpEndpoint', f'http://localhost:{args.port}'] + storeArgs
log('broker: ' + ' '.join(cmd))
brokerLog = open(os.path.join(args.work, 'broker.log'), 'w')
broker = subprocess.Popen(cmd, stdout=brokerLog, stderr=subprocess.STDOUT)


def request(conn, method, path, body=None, headers=None):
    h = {'Content-Type': 'application/json'} if body is not None else {}
    h.update(headers or {})
    conn.request(method, path, body=json.dumps(body) if body is not None else None, headers=h)
    r = conn.getresponse()
    data = r.read()
    return r.status, r.headers, data


def newConn():
    return http.client.HTTPConnection('localhost', args.port, timeout=30)


for i in range(300):
    try:
        c = newConn()
        st, _, _ = request(c, 'GET', '/version')
        c.close()
        if st == 200:
            break
    except OSError:
        pass
    if broker.poll() is not None:
        log('the broker exited during startup')
        sys.exit(1)
    time.sleep(0.1)
else:
    log('no broker on port %d' % args.port)
    sys.exit(1)

conn = newConn()
for name, trig, extra in (('created', 'entityCreated', {}),
                          ('updated', 'attributeUpdated', {'watchedAttributes': ['speed']}),
                          ('deleted', 'entityDeleted', {})):
    sub = {'id': f'urn:ngsi-ld:Subscription:soak-{name}', 'type': 'Subscription', 'entities': [{'type': T}],
           'notificationTrigger': [trig],
           'notification': {'format': 'keyValues', 'attributes': ['speed'],
                            'endpoint': {'uri': f'http://localhost:{args.receiverPort}/n/{name}', 'accept': 'application/json'}}}
    sub.update(extra)
    st, _, body = request(conn, 'POST', '/ngsi-ld/v1/subscriptions', sub)
    if st != 201:
        log(f'subscription {name}: {st} {body[:300]}')
        sys.exit(1)
conn.close()

#
# The load
#
expected = {'created': 0, 'updated': 0, 'deleted': 0}
counts = {}
stop = threading.Event()
models = []


def entity(eid, speed):
    # The fixture entity of test/perf/perfRun.sh, ~550 bytes
    return {'id': eid, 'type': T,
            'brand': {'type': 'Property', 'value': 'Mercedes'},
            'speed': {'type': 'Property', 'value': speed, 'observedAt': '2026-08-20T10:00:00Z'},
            'location': {'type': 'GeoProperty', 'value': {'type': 'Point', 'coordinates': [13.4, 52.5]}},
            'isParked': {'type': 'Relationship', 'object': 'urn:ngsi-ld:OffStreetParking:1'},
            'description': {'type': 'Property', 'value': 'a five-attribute vehicle used for a soak test, padded to roughly five hundred bytes ' + '-' * 60}}


OPS = [('create', 14), ('patch', 30), ('retrieve', 12), ('query', 12), ('batchCreate', 4),
       ('batchUpdate', 6), ('delete', 12), ('batchDelete', 4), ('subChurn', 2), ('types', 4)]
OPNAMES = [o for o, _ in OPS]
OPWEIGHTS = [w for _, w in OPS]


def worker(k):
    rnd = random.Random(k)
    model = {}                      # id -> speed
    models.append(model)
    seq = 0
    value = 0
    c = newConn()
    period = args.workers / args.rate
    nextAt = time.monotonic()

    def call(op, method, path, body, want):
        nonlocal c
        try:
            st, hdr, data = request(c, method, path, body)
        except (OSError, http.client.HTTPException) as e:
            fail('alive', f'{op} {method} {path}: {e!r}')
            try:
                c.close()
            except Exception:
                pass
            c = newConn()
            return None, None, None
        with lock:
            counts[op] = counts.get(op, 0) + 1
        if st not in want:
            fail('statuses', f'{op} {method} {path}: {st} (expected {want}) {data[:300]!r}')
            return None, None, None
        return st, hdr, data

    def newId():
        nonlocal seq
        seq += 1
        return f'urn:ngsi-ld:{T}:w{k}-{seq}'

    def nextValue():
        nonlocal value
        value += 1
        return value

    while not stop.is_set():
        nextAt += period
        delay = nextAt - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        op = rnd.choices(OPNAMES, OPWEIGHTS)[0]
        ids = list(model)
        if op in ('create', 'batchCreate') and len(model) >= args.maxEntities:
            op = 'delete' if op == 'create' else 'batchDelete'
        if op in ('patch', 'retrieve', 'delete', 'batchUpdate', 'batchDelete') and len(ids) < 5:
            op = 'create'

        if op == 'create':
            eid, v = newId(), nextValue()
            if call(op, 'POST', '/ngsi-ld/v1/entities', entity(eid, v), (201,))[0]:
                model[eid] = v
                with lock:
                    expected['created'] += 1
        elif op == 'batchCreate':
            batch = [(newId(), nextValue()) for _ in range(5)]
            if call(op, 'POST', '/ngsi-ld/v1/entityOperations/create', [entity(e, v) for e, v in batch], (201,))[0]:
                model.update(batch)
                with lock:
                    expected['created'] += len(batch)
        elif op == 'patch':
            eid, v = rnd.choice(ids), nextValue()
            if call(op, 'PATCH', f'/ngsi-ld/v1/entities/{eid}/attrs',
                    {'speed': {'type': 'Property', 'value': v, 'observedAt': '2026-08-20T10:00:00Z'}}, (204,))[0]:
                model[eid] = v
                with lock:
                    expected['updated'] += 1
        elif op == 'batchUpdate':
            batch = [(e, nextValue()) for e in rnd.sample(ids, 5)]
            body = [{'id': e, 'type': T, 'speed': {'type': 'Property', 'value': v, 'observedAt': '2026-08-20T10:00:00Z'}} for e, v in batch]
            if call(op, 'POST', '/ngsi-ld/v1/entityOperations/update', body, (204,))[0]:
                model.update(batch)
                with lock:
                    expected['updated'] += len(batch)
        elif op == 'retrieve':
            eid = rnd.choice(ids)
            st, _, data = call(op, 'GET', f'/ngsi-ld/v1/entities/{eid}?options=keyValues&attrs=speed', None, (200,))
            if st:
                got = json.loads(data).get('speed')
                if got != model[eid]:
                    fail('reads', f'{eid}: speed {got}, last written {model[eid]}')
        elif op == 'query':
            st, hdr, _ = call(op, 'GET', f'/ngsi-ld/v1/entities?type={T}&q=speed%3E{max(0, value - 50)}&limit=20&count=true', None, (200,))
            if st and hdr.get('NGSILD-Results-Count') is None:
                fail('statuses', 'query: no NGSILD-Results-Count')
        elif op == 'delete':
            eid = rnd.choice(ids)
            if call(op, 'DELETE', f'/ngsi-ld/v1/entities/{eid}', None, (204,))[0]:
                del model[eid]
                with lock:
                    expected['deleted'] += 1
        elif op == 'batchDelete':
            batch = rnd.sample(ids, 3)
            if call(op, 'POST', '/ngsi-ld/v1/entityOperations/delete', batch, (204,))[0]:
                for e in batch:
                    del model[e]
                with lock:
                    expected['deleted'] += len(batch)
        elif op == 'subChurn':
            sid = f'urn:ngsi-ld:Subscription:soak-churn-{k}-{seq}'
            seq += 1
            sub = {'id': sid, 'type': 'Subscription', 'entities': [{'type': 'SoakNothing'}],
                   'notification': {'endpoint': {'uri': f'http://localhost:{args.receiverPort}/n/churn'}}}
            if call(op, 'POST', '/ngsi-ld/v1/subscriptions', sub, (201,))[0]:
                call(op, 'DELETE', f'/ngsi-ld/v1/subscriptions/{sid}', None, (204,))
        elif op == 'types':
            call(op, 'GET', '/ngsi-ld/v1/types', None, (200,))
    c.close()


#
# Memory: RssAnon, RssFile, VmRSS of the broker, every --sampleSeconds
#
samples = []                 # (seconds since start, RssAnon KiB, RssFile KiB, VmRSS KiB)


def rss():
    vals = {}
    with open(f'/proc/{broker.pid}/status') as f:
        for line in f:
            m = re.match(r'^(RssAnon|RssFile|VmRSS):\s+(\d+) kB', line)
            if m:
                vals[m.group(1)] = int(m.group(2))
    return vals.get('RssAnon', 0), vals.get('RssFile', 0), vals.get('VmRSS', 0)


duration = args.minutes * 60
log(f'{args.db}: {args.minutes:g} minutes at {args.rate:g} requests/s, {args.workers} workers, '
    f'at most {args.maxEntities} live entities each')
threads = [threading.Thread(target=worker, args=(k,), daemon=True) for k in range(args.workers)]
t0 = time.monotonic()
for t in threads:
    t.start()

nextReport = 0
while time.monotonic() - t0 < duration:
    time.sleep(min(args.sampleSeconds, max(0.1, duration - (time.monotonic() - t0))))
    if broker.poll() is not None:
        fail('alive', f'the broker exited with {broker.returncode} after {time.monotonic() - t0:.0f} s')
        break
    s = (time.monotonic() - t0,) + rss()
    samples.append(s)
    if s[0] >= nextReport:
        with lock:
            n = sum(counts.values())
        log(f'{s[0] / 60:6.1f} min  {n} requests  RssAnon {s[1] / 1024:.1f} MiB  RssFile {s[2] / 1024:.1f} MiB  '
            f'live {sum(len(m) for m in models)}  notified {notified}')
        nextReport += 600
stop.set()
for t in threads:
    t.join(60)

with open(os.path.join(args.work, 'rss.csv'), 'w') as f:
    f.write('seconds,rssAnonKiB,rssFileKiB,vmRssKiB\n')
    for s in samples:
        f.write('%.0f,%d,%d,%d\n' % s)

alive = broker.poll() is None
results = []                 # (check, ok, detail)

#
# Notifications: the count expected, given up to a minute to drain
#
if alive:
    for i in range(120):
        with lock:
            if all(notified[k] >= expected[k] for k in expected):
                break
        time.sleep(0.5)
    time.sleep(2)            # anything more than expected arrives too
with lock:
    detail = ', '.join(f'{k} {notified[k]}/{expected[k]}' for k in expected)
    okN = all(notified[k] == expected[k] for k in expected) and notified['other'] == 0
results.append(('notified', okN, detail + ('' if notified['other'] == 0 else f', unexpected {notified["other"]}')))

#
# Memory: the warm-up's end against the run's end
#
warm = max(300, 0.15 * duration)
after = [s for s in samples if s[0] >= warm]
if len(after) >= 10:
    base = statistics.median(s[1] for s in after[:5]) / 1024
    end = statistics.median(s[1] for s in after[-5:]) / 1024
    allowed = max(base * args.rssMaxGrowthPct / 100, args.rssMaxGrowthMiB)
    results.append(('memory', end - base <= allowed,
                    f'RssAnon {base:.1f} -> {end:.1f} MiB after the warm-up ({end - base:+.1f}, allowed +{allowed:.1f}); '
                    f'peak VmRSS {max(s[3] for s in samples) / 1024:.1f} MiB'))
else:
    results.append(('memory', False, f'too few samples after the warm-up ({len(after)}) - a run shorter than ~{(warm + 10 * args.sampleSeconds) / 60:.0f} min cannot tell'))

#
# The final read: every live entity, its speed, the count, the subscriptions
#
if alive:
    c = newConn()
    model = {}
    for m in models:
        model.update(m)
    bad = 0
    for eid, v in model.items():
        try:
            st, _, data = request(c, 'GET', f'/ngsi-ld/v1/entities/{eid}?options=keyValues&attrs=speed')
        except (OSError, http.client.HTTPException) as e:
            fail('alive', f'final read {eid}: {e!r}')
            c = newConn()
            bad += 1
            continue
        if st != 200 or json.loads(data).get('speed') != v:
            bad += 1
            if bad <= 10:
                log(f'      {eid}: {st} {data[:200]!r}, expected speed {v}')
    st, hdr, _ = request(c, 'GET', f'/ngsi-ld/v1/entities?type={T}&count=true&limit=1&options=keyValues')
    count = int(hdr.get('NGSILD-Results-Count', -1)) if st == 200 else -1
    st, _, data = request(c, 'GET', '/ngsi-ld/v1/subscriptions?limit=100')
    subs = sorted(s['id'] for s in json.loads(data)) if st == 200 else []
    wantSubs = sorted(f'urn:ngsi-ld:Subscription:soak-{n}' for n in ('created', 'updated', 'deleted'))
    c.close()
    results.append(('consistent', bad == 0 and count == len(model) and subs == wantSubs,
                    f'{len(model)} live entities, {bad} wrong; count {count}; subscriptions {len(subs)} (expected 3)'))
else:
    results.append(('consistent', False, 'the broker is gone'))

#
# Stop, then the log
#
if alive:
    broker.terminate()
    try:
        broker.wait(60)
    except subprocess.TimeoutExpired:
        broker.kill()
        fail('alive', 'the broker did not stop within 60 s of SIGTERM')
brokerLog.close()
errs = [line.rstrip() for line in open(os.path.join(args.work, 'broker.log'), errors='replace')
        if re.match(r'^(E|X)[:(]', line) and not re.search(r': 4[0-9][0-9] ', line)]
results.append(('log', not errs, f'{len(errs)} E/X line(s)' + (': ' + errs[0][:200] if errs else '')))

with lock:
    total = sum(counts.values())
byCheck = {}
for check, msg in FAILS:
    byCheck.setdefault(check, []).append(msg)
for check in ('alive', 'statuses', 'reads'):
    msgs = byCheck.get(check, [])
    results.insert(0, (check, not msgs, f'{len(msgs)} failure(s)' + (': ' + msgs[0][:200] if msgs else '')))
results.insert(0, ('load', True, f'{total} requests in {args.minutes:g} min: ' +
                   ', '.join(f'{o} {counts.get(o, 0)}' for o in OPNAMES)))

nFailed = 0
with open(os.path.join(args.work, 'summary.md'), 'w') as f:
    f.write(f'| {args.db} | | |\n|---|---|---|\n')
    for check, ok, detail in results:
        nFailed += 0 if ok else 1
        print(f'{"ok  " if ok else "FAIL"}  {check:10} {detail}', flush=True)
        f.write(f'| {check} | {"✅" if ok else "❌"} | {detail.replace("|", "/")} |\n')
receiver.shutdown()
sys.exit(nFailed)
