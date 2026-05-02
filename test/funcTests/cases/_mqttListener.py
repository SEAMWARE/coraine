#!/usr/bin/env python3
"""Tiny MQTT subscriber for functests.

Subscribes to a topic on a given broker and appends each received
message (one per line, JSON-as-received) to an output file.

Usage:  _mqttListener.py <port> <topic> <out-file>
"""
import sys
import paho.mqtt.client as mqtt

if len(sys.argv) != 4:
    sys.stderr.write("usage: _mqttListener.py <port> <topic> <out-file>\n")
    sys.exit(2)

port    = int(sys.argv[1])
topic   = sys.argv[2]
outFile = sys.argv[3]

# Truncate
open(outFile, 'w').close()

def on_connect(c, *_):
    c.subscribe(topic)

def on_message(c, _ud, msg):
    with open(outFile, 'a') as f:
        f.write(msg.payload.decode('utf-8', errors='replace') + "\n")
        f.flush()

c = mqtt.Client()
c.on_connect = on_connect
c.on_message = on_message
c.connect("localhost", port, 60)
c.loop_forever()
