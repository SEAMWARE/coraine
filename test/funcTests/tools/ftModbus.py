#!/usr/bin/env python3
#
# FILE            ftModbus.py
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# A Modbus TCP device for the functests - the four tables, and an HTTP port to drive it:
#
#   --port N        Modbus TCP on N, the control API on N+1
#
#   GET  /get?table=holding&addr=0&count=2      the values, as a JSON array
#   POST /set     {"table":"holding","addr":0,"values":[17096,0]}
#   POST /mode    {"mode":"normal"} | {"mode":"silent"} | {"mode":"exception","code":2}
#   GET  /writes  how many write requests (fc 5, 6, 15, 16) it has received
#
# silent: requests are read and never answered (a device that is gone); exception: every request is
# answered with that exception code. Function codes 1-6, 15, 16.
#
import json
import socketserver
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

tables = {name: [0] * 65536 for name in ("coil", "discrete", "holding", "input")}
state  = {"mode": "normal", "code": 2, "writes": 0}
lock   = threading.Lock()


def handle_pdu(pdu):
    fc = pdu[0]
    with lock:
        if state["mode"] == "exception":
            return bytes([fc | 0x80, state["code"]])

        if fc in (1, 2, 3, 4):
            addr, count = struct.unpack(">HH", pdu[1:5])
            if fc in (1, 2):
                table = tables["coil" if fc == 1 else "discrete"]
                if addr + count > 65536:
                    return bytes([fc | 0x80, 2])
                bits = table[addr:addr + count]
                out  = bytearray((count + 7) // 8)
                for i, b in enumerate(bits):
                    if b:
                        out[i // 8] |= 1 << (i % 8)
                return bytes([fc, len(out)]) + bytes(out)
            table = tables["holding" if fc == 3 else "input"]
            if addr + count > 65536:
                return bytes([fc | 0x80, 2])
            regs = table[addr:addr + count]
            return bytes([fc, count * 2]) + b"".join(struct.pack(">H", r & 0xFFFF) for r in regs)

        if fc == 5:
            addr, value = struct.unpack(">HH", pdu[1:5])
            tables["coil"][addr] = 1 if value == 0xFF00 else 0
            state["writes"] += 1
            return pdu[:5]

        if fc == 6:
            addr, value = struct.unpack(">HH", pdu[1:5])
            tables["holding"][addr] = value
            state["writes"] += 1
            return pdu[:5]

        if fc == 15:
            addr, count, nbytes = struct.unpack(">HHB", pdu[1:6])
            data = pdu[6:6 + nbytes]
            for i in range(count):
                tables["coil"][addr + i] = (data[i // 8] >> (i % 8)) & 1
            state["writes"] += 1
            return pdu[:5]

        if fc == 16:
            addr, count, nbytes = struct.unpack(">HHB", pdu[1:6])
            for i in range(count):
                tables["holding"][addr + i] = struct.unpack(">H", pdu[6 + 2 * i:8 + 2 * i])[0]
            state["writes"] += 1
            return pdu[:5]

        return bytes([fc | 0x80, 1])                 # illegal function


class ModbusHandler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        while True:
            header = b""
            while len(header) < 7:
                chunk = sock.recv(7 - len(header))
                if not chunk:
                    return
                header += chunk
            tid, proto, length, unit = struct.unpack(">HHHB", header)
            pdu = b""
            while len(pdu) < length - 1:
                chunk = sock.recv(length - 1 - len(pdu))
                if not chunk:
                    return
                pdu += chunk
            with lock:
                silent = state["mode"] == "silent"
            if silent:
                continue
            resp = handle_pdu(pdu)
            sock.sendall(struct.pack(">HHHB", tid, 0, len(resp) + 1, unit) + resp)


class ControlHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        q   = parse_qs(url.query)
        if url.path == "/get":
            table = q.get("table", ["holding"])[0]
            addr  = int(q.get("addr", ["0"])[0])
            count = int(q.get("count", ["1"])[0])
            with lock:
                self.reply(200, tables[table][addr:addr + count])
        elif url.path == "/writes":
            with lock:
                self.reply(200, state["writes"])
        else:
            self.reply(404, {"error": "no such path"})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")
        if self.path == "/set":
            with lock:
                for i, v in enumerate(body["values"]):
                    tables[body["table"]][body["addr"] + i] = v
            self.reply(200, {"ok": True})
        elif self.path == "/mode":
            with lock:
                state["mode"] = body["mode"]
                state["code"] = body.get("code", 2)
            self.reply(200, {"ok": True})
        else:
            self.reply(404, {"error": "no such path"})


def main():
    port = 7720
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    socketserver.ThreadingTCPServer.daemon_threads      = True
    modbus  = socketserver.ThreadingTCPServer(("127.0.0.1", port), ModbusHandler)
    control = ThreadingHTTPServer(("127.0.0.1", port + 1), ControlHandler)
    threading.Thread(target=modbus.serve_forever, daemon=True).start()
    control.serve_forever()


if __name__ == "__main__":
    main()
