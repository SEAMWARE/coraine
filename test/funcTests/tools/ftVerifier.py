#!/usr/bin/env python3
#
# FILE            ftVerifier.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# A stand-in for the Data Space Connector's VCVerifier and ODRL-PAP, for the auth functests - the routes
# fdsc.so reads, answered as VCVerifier 6.23.1 and odrl-pap 1.7.1 answer them (the standard library only):
#
#   GET /services/<id>/.well-known/openid-configuration   the discovery document: issuer, jwks_uri, token_endpoint
#   GET /.well-known/jwks                                 DIR/jwks.json
#   GET /policy?page=N&pageSize=M                         DIR/papListing.json, paged as the PAP pages (default 0, 25)
#   GET /count                                            how many times each of those was asked, {path: n}
#
#   --port N        listen on N
#   --dir DIR       where jwks.json and papListing.json are (read at every request: replace them to change them)
#   --issuer URL    the issuer (default http://localhost:<port>)
#
import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

counts = {}
countLock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def answer(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def file(self, name):
        try:
            with open(os.path.join(self.server.dir, name)) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def do_GET(self):
        url = urlparse(self.path)
        path = url.path

        if path == "/count":
            with countLock:
                return self.answer(200, dict(counts))

        key = "/services/*/.well-known/openid-configuration" if path.endswith("/.well-known/openid-configuration") else path
        with countLock:
            counts[key] = counts.get(key, 0) + 1

        if path.startswith("/services/") and path.endswith("/.well-known/openid-configuration"):
            service = path.split("/")[2]
            iss = self.server.issuer
            return self.answer(200, {
                "issuer": iss,
                "authorization_endpoint": iss + "/api/v2/loginQR",
                "token_endpoint": iss + "/services/" + service + "/token",
                "jwks_uri": iss + "/.well-known/jwks",
                "grant_types_supported": ["authorization_code", "vp_token", "urn:ietf:params:oauth:grant-type:token-exchange"],
                "id_token_signing_alg_values_supported": ["RS256"],
            })

        if path == "/.well-known/jwks":
            jwks = self.file("jwks.json")
            return self.answer(200, jwks) if jwks is not None else self.answer(404, {"error": "no keys"})

        if path == "/policy":
            policies = self.file("papListing.json")
            if policies is None:
                return self.answer(500, {"error": "no policies"})
            q = parse_qs(url.query)
            page = int(q.get("page", ["0"])[0])
            size = int(q.get("pageSize", ["25"])[0])
            return self.answer(200, policies[page * size:(page + 1) * size])

        self.answer(404, {"error": "not found"})


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--dir", required=True)
    p.add_argument("--issuer")
    a = p.parse_args()

    server = ThreadingHTTPServer(("localhost", a.port), Handler)
    server.dir = a.dir
    server.issuer = a.issuer or ("http://localhost:%d" % a.port)
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
