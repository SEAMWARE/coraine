#!/usr/bin/env bash
#
# etsiForgeFree.sh - run the ETSI suite without fetching its @context from forge.etsi.org
#
# The suite's payloads and expectations name its @context by URL - on forge
# (https://forge.etsi.org/rep/cim/ngsi-ld-test-suite/-/raw/<branch>/resources/jsonld-contexts/...),
# 345 files of them. The broker downloads that URL on nearly every request, so the
# day forge answers 500 every test fails on 504 LdContextNotAvailable - the whole
# nightly ETSI job on 2026-09-26, with no broker change.
#
# So the contexts live HERE (test/etsi/contexts - copies of the suite's
# resources/jsonld-contexts, integration/all-fixes da1ce52e), are pushed to the
# context server, and the CLONED suite is rewritten to name the context server
# instead. The rewrite is done in the clone at run time and never committed:
# committed to the suite fork it would collide with every upstream merge.
#
# The compound context names its sibling RELATIVELY ("ngsi-ld-test-suite.jsonld"),
# so both files side by side on the context server resolve as they do on forge.
#
# Update the copies when the suite's contexts change - the ETSI run then tells:
# a term the copy lacks shows up as expanded where the expectation has it compact.
#
# The suite's own ngsild_test_suite_context (resources/variables.py) is built from
# two lines the rewrite does not see - pass it to robot:
#   --variable ngsild_test_suite_context:<context-server>/jsonldContexts/ngsi-ld-test-suite-compound.jsonld
#
# Usage:  etsiForgeFree.sh <suite-dir> <context-server-base-url>
#   e.g.  etsiForgeFree.sh etsi-suite http://localhost:7080
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
set -e

SUITE=${1:?usage: etsiForgeFree.sh <suite-dir> <context-server-base-url>}
CS=${2:?usage: etsiForgeFree.sh <suite-dir> <context-server-base-url>}
CONTEXTS=$(cd "$(dirname "$0")" && pwd)/contexts

#
# Push. A re-run finds them there already (409) - replace them, so the server
# always holds what this checkout has.
#
for f in "$CONTEXTS"/*.jsonld
do
  url="$CS/jsonldContexts/$(basename "$f")"
  code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/ld+json' --data-binary @"$f" "$url")

  if [ "$code" = 409 ]
  then
    curl -s -o /dev/null -X DELETE "$url"
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/ld+json' --data-binary @"$f" "$url")
  fi

  case "$code" in
    2*) echo "  served: $url" ;;
    *)  echo "etsiForgeFree.sh: pushing $f to $url failed: HTTP $code" >&2; exit 1 ;;
  esac
done

#
# Rewrite. Text files only (-I): a compiled __pycache__/*.pyc also carries the URL,
# and sed on bytecode breaks the import of resources/variables.py.
#
FORGE='https://forge\.etsi\.org/rep/cim/ngsi-ld-test-suite/-/raw/[^/"'"'"' ]+/resources/jsonld-contexts/'

cd "$SUITE"
files=$(grep -rlIE "$FORGE" --exclude-dir=.git --exclude-dir=.venv --exclude-dir=__pycache__ . || true)

if [ -n "$files" ]
then
  echo "$files" | xargs sed -i -E "s#$FORGE#$CS/jsonldContexts/#g"
fi
echo "  rewritten: $(echo "$files" | grep -c .) files"

left=$(grep -rnIE "$FORGE" --exclude-dir=.git --exclude-dir=.venv --exclude-dir=__pycache__ . | wc -l)
if [ "$left" -ne 0 ]
then
  echo "etsiForgeFree.sh: $left forge @context reference(s) left in $SUITE" >&2
  exit 1
fi
