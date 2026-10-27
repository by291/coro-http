#!/usr/bin/env bash
#
# Usage:
#   bench/bench.sh [url] [requests] [concurrency]
#
# Prefers wrk when it is installed, falls back to ApacheBench.
set -euo pipefail

URL="${1:-http://127.0.0.1:8080/hello}"
REQUESTS="${2:-20000}"
CONCURRENCY="${3:-100}"

echo "target: $URL"
echo "requests: $REQUESTS  concurrency: $CONCURRENCY"
echo

if command -v wrk >/dev/null 2>&1; then
  wrk -t4 -c"$CONCURRENCY" -d10s "$URL"
else
  ab -n "$REQUESTS" -c "$CONCURRENCY" -k "$URL"
fi
