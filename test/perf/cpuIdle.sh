#!/bin/bash
#
# cpuIdle.sh <shallow|deep|show> - the CPU idle states latency benchmarks depend on
#
#   shallow  disable idle states deeper than ~10 us (C2, C3 here): a core wakes fast, so a request
#            relayed through several brokers is not paying a deep-sleep wakeup per hop
#   deep     re-enable them all - the machine's normal state
#   show     which are disabled now
#
# A quiet machine sleeps deeper between requests: the 1-connection cor:// chain measured 11,000 req/s
# with deep states and 14,200 without (doc/cor-protocol.md § 6.1). Compare transports only under the
# same setting, and say which.
#
# Needs root, through sudo without a password for exactly these two commands - one line, installed
# with `sudo visudo -f /etc/sudoers.d/cpupower-bench`:
#
#   <user> ALL=(root) NOPASSWD: /usr/bin/cpupower idle-set -D 10, /usr/bin/cpupower idle-set -E
#
# (Not a setuid script: Linux ignores the s-bit on interpreted scripts.)
#
case "$1" in
  shallow) sudo -n /usr/bin/cpupower idle-set -D 10 > /dev/null || { echo "cpuIdle.sh: sudo rule missing - see the header"; exit 1; } ;;
  deep)    sudo -n /usr/bin/cpupower idle-set -E    > /dev/null || { echo "cpuIdle.sh: sudo rule missing - see the header"; exit 1; } ;;
  show)    ;;
  *)       echo "usage: $0 shallow|deep|show"; exit 2 ;;
esac

disabled=$(cpupower idle-info 2>/dev/null | grep -c DISABLED)
if [ "$disabled" -gt 0 ]; then
  echo "cpu idle: shallow ($(cpupower idle-info 2>/dev/null | grep DISABLED | sed 's/ *(DISABLED).*//' | tr '\n' ' ')disabled)"
else
  echo "cpu idle: deep (all idle states enabled)"
fi
