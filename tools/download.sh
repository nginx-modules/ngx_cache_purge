#!/bin/sh
# Compatible with Ubuntu 20.04 curl. Retry transport failures, not HTTP 4xx.
set -eu
attempt=1
while :; do
    rc=0
    curl --fail --location --connect-timeout 20 --max-time 180 \
         --retry 2 --retry-max-time 180 "$1" -o "$2" || rc=$?
    [ "$rc" -ne 0 ] || exit 0
    case "$rc" in 5|6|7|28|35|52|56) ;; *) exit "$rc" ;; esac
    [ "$attempt" -lt 3 ] || exit "$rc"
    sleep "$attempt"
    attempt=$((attempt + 1))
done
