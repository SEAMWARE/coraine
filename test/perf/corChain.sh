#!/bin/bash
#
# corChain.sh <http|cor|cor-all> - the cor:// benchmark: three brokers A -> B -> C, GET on A
#
#   http     every hop HTTP; the client is wrk
#   cor      the two forwarding hops cor://; the client is still wrk, over HTTP
#   cor-all  nothing but cor://: the client too is cor:// - corRequest's load mode
#
# C holds the entities; A and B each register the next broker - over cor:// or http:// - so every
# request measured is client -> A -> B -> C -> B -> A -> client. corDB, no history, each broker on
# its own two cores, wrk on 8-15. Run both modes back to back and compare:
#
#   CORAINE_BIN=BUILD_RELEASE/src/app/coraine/coraine SEAMWARE_PLUGIN_DIR=<release plugins> ./corChain.sh http
#   ... ./corChain.sh cor
#
# (doc/cor-protocol.md § 6.1 has the numbers of the first run.)
#
MODE=$1
echo "== corChain $MODE - $($(dirname $0)/cpuIdle.sh show)"
B=${CORAINE_BIN:-/opt/seamware/bin/coraine}
start() { taskset -c $3 $B --port $1 --corPort $2 --database corDB --troe none -dist --foreground $CORAINE_ARGS > /dev/null 2>&1 & echo $!; }
PA=$(start 9201 9301 0-1); PB=$(start 9202 9302 2-3); PC=$(start 9203 9303 4-5)
for p in 9201 9202 9203; do for i in $(seq 1 50); do curl -s -o /dev/null http://localhost:$p/version && break; sleep 0.1; done; done
J='Content-Type: application/json'
curl -s -o /dev/null -X POST localhost:9203/ngsi-ld/v1/entities -H "$J" -d '{"id":"urn:E1","type":"Vehicle","speed":{"type":"Property","value":42,"observedAt":"2026-10-01T12:00:00.000Z"},"owner":{"type":"Relationship","object":"urn:Person:P1"},"name":{"type":"LanguageProperty","languageMap":{"en":"truck","es":"camion"}},"location":{"type":"GeoProperty","value":{"type":"Point","coordinates":[-3.7,40.4]}}}'
big='{"id":"urn:E20","type":"Vehicle"'; for a in $(seq 1 20); do big="$big,\"attr$a\":{\"type\":\"Property\",\"value\":$a.5,\"observedAt\":\"2026-10-01T12:00:00.000Z\",\"unitCode\":\"CEL\"}"; done; big="$big}"
curl -s -o /dev/null -X POST localhost:9203/ngsi-ld/v1/entities -H "$J" -d "$big"
if [ "$MODE" != http ]; then EPB=cor://localhost:9302; EPC=cor://localhost:9303; else EPB=http://localhost:9202; EPC=http://localhost:9203; fi
reg() { curl -s -o /dev/null -X POST localhost:$1/ngsi-ld/v1/csourceRegistrations -H "$J" -d '{"id":"urn:Reg:'$1'","type":"ContextSourceRegistration","endpoint":"'$2'","mode":"inclusive","information":[{"entities":[{"type":"Vehicle"}]}]}'; }
reg 9201 $EPB; reg 9202 $EPC
CR=${CORREQUEST:-$(dirname $0)/../funcTests/corRequest/corRequest}
for e in E1 E20; do
  U=http://localhost:9201/ngsi-ld/v1/entities/urn:$e
  if [ "$MODE" = cor-all ]; then
    out=""
    for c in 1 16; do
      taskset -c 8-15 $CR --url cor://localhost:9301 --path /ngsi-ld/v1/entities/urn:$e -c $c --duration 2 > /dev/null < /dev/null
      r=$(taskset -c 8-15 $CR --url cor://localhost:9301 --path /ngsi-ld/v1/entities/urn:$e -c $c --duration 8 < /dev/null)
      field() { echo "$r" | awk -v k="$1" -v o="$2" '{ for (i = 1; i <= NF; i++) if ($i == k) print $(i + o) }'; }
      out="$out $(field req/s -1) $(field p50 1)us $(field p99 1)us"
    done
    set -- $out
    printf "%-7s urn:%-4s  c1: %8s req/s  p50 %-9s p99 %-9s |  c16: %8s req/s  p50 %-9s p99 %s\n" $MODE $e $1 $2 $3 $4 $5 $6
    continue
  fi
  curl -s $U | head -c 60 > /dev/null
  taskset -c 8-15 wrk -t1 -c1 -d2s $U > /dev/null 2>&1
  r1=$(taskset -c 8-15 wrk -t1 -c1  -d8s --latency $U | awk '/Requests\/sec/{rps=$2} /^ +50%/{p50=$2} /^ +99%/{p99=$2} END{print rps, p50, p99}')
  r16=$(taskset -c 8-15 wrk -t4 -c16 -d8s --latency $U | awk '/Requests\/sec/{rps=$2} /^ +50%/{p50=$2} /^ +99%/{p99=$2} END{print rps, p50, p99}')
  set -- $r1; a1=$1; a2=$2; a3=$3; set -- $r16
  printf "%-7s urn:%-4s  c1: %8s req/s  p50 %-9s p99 %-9s |  c16: %8s req/s  p50 %-9s p99 %s\n" $MODE $e $a1 $a2 $a3 $1 $2 $3
done
kill $PA $PB $PC
# the brokers are not this shell's children (started in $(...)): wait for their ports to be free
for i in $(seq 1 100); do ss -ltn | grep -qE ':(920[123]|930[123]) ' || break; sleep 0.1; done
