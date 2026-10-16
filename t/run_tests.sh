#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
[ "$#" -gt 0 ] || set -- t/

# Use the same prove invocation locally and in CI; format its actual result.
if [ -n "${TEST_REPORT_DIR:-}" ]; then
    mkdir -p "$TEST_REPORT_DIR"
    report="$TEST_REPORT_DIR/${TEST_REPORT_NAME:-tests}.xml"
    status=0
    raw="${report%.xml}.stdout.log"
    errors="${report%.xml}.stderr.log"
    prove -v --formatter=TAP::Formatter::JUnit "$@" > "$raw" 2> "$errors" || status=$?
    report_status=0
    perl t/report_tap.pl "$raw" "$errors" "$report" "$status" || report_status=$?
    cat "$raw" "$errors"
    [ "$status" -ne 0 ] || status=$report_status
    exit "$status"
fi

exec prove -v "$@"
