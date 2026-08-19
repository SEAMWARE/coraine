#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# setup-mongo8.sh - install MongoDB 8.0 side-by-side with MongoDB 4.4
#
# MongoDB 4.4 stays on port 27017, untouched
# MongoDB 8.0 runs on port 27018 — the port Coraine is pointed at
#

set -e

# Config file
cat > /tmp/mongod8.conf <<ENDOFCONF
storage:
  dbPath: /var/lib/mongodb8

systemLog:
  destination: file
  logAppend: true
  path: /var/log/mongodb/mongod8.log

net:
  port: 27018
  bindIp: 127.0.0.1
ENDOFCONF
sudo cp /tmp/mongod8.conf /etc/mongod8.conf
rm /tmp/mongod8.conf

# Systemd unit
cat > /tmp/mongod8.service <<ENDOFUNIT
[Unit]
Description=MongoDB 8.0 (port 27018)
After=network-online.target
Wants=network-online.target

[Service]
User=mongodb
Group=mongodb
ExecStart=/opt/mongo8/mongod --config /etc/mongod8.conf
RuntimeDirectory=mongodb8
PIDFile=/run/mongodb8/mongod8.pid
LimitNOFILE=64000
LimitNPROC=64000

[Install]
WantedBy=multi-user.target
ENDOFUNIT
sudo cp /tmp/mongod8.service /etc/systemd/system/mongod8.service
rm /tmp/mongod8.service

# Start and enable
sudo systemctl daemon-reload
sudo systemctl start mongod8
sudo systemctl enable mongod8

# Verify
echo "MongoDB 8.0 status:"
sudo systemctl is-active mongod8
echo ""
echo "Ping test:"
mongosh --port 27018 --quiet --eval "db.runCommand({ping:1})"
