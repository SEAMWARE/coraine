#!/usr/bin/env python3
#
# FILE            ftWs.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# A WebSocket client for the functests (RFC 6455, the standard library only): it opens ONE WebSocket to
# the broker and keeps it, and an HTTP port drives it:
#
#   --broker HOST:PORT   the broker (GET /ngsi-ld/v1/ws, Upgrade: websocket)
#   --port N             the control API
#   --protocol P         Sec-WebSocket-Protocol to ask for (default: none)
#   --tenant T           NGSILD-Tenant of the upgrade request - the connection's tenant (default: none)
#   --first JSON         a message sent in the same write as the upgrade request - before the 101: the
#                        bytes a server must hand over with the socket
#
#   POST /send       the body, sent as one text message (masked, as a client must)
#   POST /sendFrag   the body, sent as a text frame and a continuation (two halves)
#   POST /ping       a ping; the pong is recorded as {"pong": "<payload>"}
#   GET  /messages   every message received so far, as a JSON array (each a parsed JSON value)
#   GET  /count      how many
#   POST /close      a close frame (1000); the broker's close is recorded as {"close": <status>}
#
import base64
import json
import os
import socket
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

received = []
lock     = threading.Lock()
sock     = None


def recv_exact(n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def frame(opcode, payload, fin=True):
    mask   = os.urandom(4)
    header = bytes([(0x80 if fin else 0) | opcode])
    n      = len(payload)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return header + mask + masked


def send_frame(opcode, payload, fin=True):
    sock.sendall(frame(opcode, payload, fin))


def reader():
    message = b""
    try:
        while True:
            b0, b1 = recv_exact(2)
            opcode = b0 & 0x0F
            n      = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", recv_exact(8))[0]
            payload = recv_exact(n) if n > 0 else b""
            if opcode in (0x1, 0x0):
                message += payload
                if b0 & 0x80:
                    with lock:
                        try:
                            received.append(json.loads(message.decode()))
                        except ValueError:
                            received.append({"text": message.decode(errors="replace")})
                    message = b""
            elif opcode == 0x8:
                status = struct.unpack(">H", payload[:2])[0] if len(payload) >= 2 else None
                with lock:
                    received.append({"close": status})
                return
            elif opcode == 0xA:
                with lock:
                    received.append({"pong": payload.decode(errors="replace")})
    except (EOFError, OSError):
        with lock:
            received.append({"eof": True})


def connect(broker, protocol, first, tenant):
    global sock
    host, port = broker.split(":")
    sock = socket.create_connection((host, int(port)))
    key  = base64.b64encode(os.urandom(16)).decode()
    req  = ("GET /ngsi-ld/v1/ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n" % (broker, key))
    if protocol:
        req += "Sec-WebSocket-Protocol: %s\r\n" % protocol
    if tenant:
        req += "NGSILD-Tenant: %s\r\n" % tenant
    sock.sendall((req + "\r\n").encode() + (frame(0x1, first.encode()) if first else b""))

    head = b""
    while b"\r\n\r\n" not in head:
        chunk = sock.recv(1)
        if not chunk:
            sys.exit("ftWs: the broker closed the connection during the handshake")
        head += chunk

    status = head.split(b"\r\n", 1)[0].decode()
    if " 101 " not in status + " ":
        sys.exit("ftWs: no upgrade: " + status)

    if protocol:                                    # what the server chose, as the first message recorded
        chosen = None
        for line in head.decode().split("\r\n")[1:]:
            if line.lower().startswith("sec-websocket-protocol:"):
                chosen = line.split(":", 1)[1].strip()
        received.append({"subprotocol": chosen})

    threading.Thread(target=reader, daemon=True).start()


class Control(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def answer(self, code, body=b""):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        with lock:
            if self.path == "/messages":
                return self.answer(200, json.dumps(received).encode())
            if self.path == "/count":
                return self.answer(200, str(len(received)).encode())
        self.answer(404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/send":
            send_frame(0x1, body)
        elif self.path == "/sendFrag":
            half = len(body) // 2
            send_frame(0x1, body[:half], fin=False)
            send_frame(0x0, body[half:], fin=True)
        elif self.path == "/ping":
            send_frame(0x9, body)
        elif self.path == "/close":
            send_frame(0x8, struct.pack(">H", 1000))
        else:
            return self.answer(404)
        self.answer(204)


def main():
    args     = sys.argv[1:]
    broker   = args[args.index("--broker") + 1]
    port     = int(args[args.index("--port") + 1])
    protocol = args[args.index("--protocol") + 1] if "--protocol" in args else None
    first    = args[args.index("--first") + 1] if "--first" in args else None
    tenant   = args[args.index("--tenant") + 1] if "--tenant" in args else None

    connect(broker, protocol, first, tenant)
    ThreadingHTTPServer(("127.0.0.1", port), Control).serve_forever()


if __name__ == "__main__":
    main()
