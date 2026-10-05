#!/usr/bin/env python3
#
# FILE            ftExecutor.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# A Service Executor for the functests (Service Execution, doc/service-execution.md) - the standard
# library only. It answers the broker's invocations by their path, and it is a notification receiver
# too:
#
#   --port N             where it listens
#   --broker HOST:PORT   the broker - for the PATCHes an asynchronous service sends back
#
#   POST /echo           synchronous: 200, { "got": <the invocation's body> }
#   POST /text           synchronous: 200, text/plain "done"
#   POST /refuse         400, a ProblemDetails (BadRequestData, "brightness out of range")
#   POST /crash          500, no body
#   POST /slow           answers after 3 s: 200 {}
#   POST /async          202; then, from a thread, PATCH /ngsi-ld/v1/services/{id} on the broker:
#                        executing + progress 50, then completed + output { "done": true }
#   POST /asyncFail      202; then PATCH: failed + a ProblemDetails
#   POST /asyncHold      202; then nothing - for a cancel
#   DELETE /asyncHold/{id}   204 - cancelled
#   DELETE /noCancel/{id}    409 - cannot pre-empt
#   POST /noCancel       202; then nothing
#   POST /notify         a notification, recorded
#
#   GET  /log            every request received, a JSON array of { method, path, serviceExecution, tenant, body }
#   GET  /notifications  the notifications received, a JSON array of their bodies
#
import json
import sys
import threading
import time
import urllib.request

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

log           = []
notifications = []
lock          = threading.Lock()
broker        = "localhost:1026"


def patch(execId, tenant, body):
    req = urllib.request.Request("http://%s/ngsi-ld/v1/services/%s" % (broker, execId),
                                 data=json.dumps(body).encode(), method="PATCH",
                                 headers={"Content-Type": "application/json", "Service-Execution": execId})
    if tenant:
        req.add_header("NGSILD-Tenant", tenant)
    try:
        urllib.request.urlopen(req, timeout=5).read()
    except Exception as e:                            # noqa: BLE001 - the test sees the execution's state
        sys.stderr.write("PATCH %s: %s\n" % (execId, e))


def asyncDone(execId, tenant):
    time.sleep(0.2)
    patch(execId, tenant, {"executionStatus": "executing", "executionProgress": {"percent": 50}})
    time.sleep(0.2)
    patch(execId, tenant, {"executionStatus": "completed", "executionOutput": {"done": True}})


def asyncFail(execId, tenant):
    time.sleep(0.2)
    patch(execId, tenant, {"executionStatus": "failed",
                           "executionError": {"type": "https://example.org/errors/Jammed", "title": "Jammed",
                                              "status": 500, "detail": "the door is jammed"}})


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def answer(self, status, body=None, contentType="application/json"):
        data = b"" if body is None else (body.encode() if isinstance(body, str) else json.dumps(body).encode())
        self.send_response(status)
        if body is not None:
            self.send_header("Content-Type", contentType)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def record(self):
        n    = int(self.headers.get("Content-Length") or 0)
        raw  = self.rfile.read(n) if n > 0 else b""
        try:
            body = json.loads(raw) if raw else None
        except ValueError:
            body = raw.decode(errors="replace")
        entry = {"method": self.command, "path": self.path, "serviceExecution": self.headers.get("Service-Execution"),
                 "tenant": self.headers.get("NGSILD-Tenant"), "body": body}
        with lock:
            log.append(entry)
        return entry

    def do_GET(self):
        with lock:
            if self.path == "/log":
                return self.answer(200, log)
            if self.path == "/notifications":
                return self.answer(200, notifications)
        self.answer(404, {})

    def do_DELETE(self):
        entry = self.record()
        if entry["path"].startswith("/asyncHold/"):
            return self.answer(204)
        if entry["path"].startswith("/noCancel/"):
            return self.answer(409, {"type": "https://example.org/errors/Busy", "title": "Busy", "status": 409})
        self.answer(404, {})

    def do_POST(self):
        entry  = self.record()
        path   = entry["path"]
        execId = entry["serviceExecution"]
        tenant = entry["tenant"]

        if path == "/notify":
            with lock:
                notifications.append(entry["body"])
            return self.answer(200)
        if path == "/echo":
            return self.answer(200, {"got": entry["body"]})
        if path == "/text":
            return self.answer(200, "done", "text/plain")
        if path == "/refuse":
            return self.answer(400, {"type": "https://uri.etsi.org/ngsi-ld/errors/BadRequestData", "title": "Bad Request Data",
                                     "status": 400, "detail": "brightness out of range"})
        if path == "/crash":
            return self.answer(500)
        if path == "/slow":
            time.sleep(3)
            return self.answer(200, {})
        if path == "/async":
            threading.Thread(target=asyncDone, args=(execId, tenant), daemon=True).start()
            return self.answer(202)
        if path == "/asyncFail":
            threading.Thread(target=asyncFail, args=(execId, tenant), daemon=True).start()
            return self.answer(202)
        if path in ("/asyncHold", "/noCancel"):
            return self.answer(202)
        self.answer(404, {})


if __name__ == "__main__":
    args   = sys.argv[1:]
    port   = int(args[args.index("--port") + 1])
    broker = args[args.index("--broker") + 1] if "--broker" in args else broker
    ThreadingHTTPServer(("localhost", port), Handler).serve_forever()
