#!/bin/bash
# Analyze module sources using the compiler commands from a real NGINX build.
set -euo pipefail

: "${NGINX_SOURCE:?Set NGINX_SOURCE to a writable NGINX source tree}"
: "${PVS_REPORT_DIR:?Set PVS_REPORT_DIR to a new report directory}"
jobs=${PVS_JOBS:-2}
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    echo 'PVS_JOBS must be a positive integer' >&2
    exit 1
fi
module=$(cd "$(dirname "$0")/.." && pwd -P)
nginx=$(cd "$NGINX_SOURCE" && pwd -P)
case "$module/" in
    "$nginx/"*)
        echo 'The module must be outside NGINX_SOURCE to avoid excluding it' >&2
        exit 1
        ;;
esac
mkdir -p "$PVS_REPORT_DIR"
reports=$(cd "$PVS_REPORT_DIR" && pwd -P)
# Refuse to publish an old report after a failed retry.
if [ -n "$(ls -A "$reports")" ]; then
    echo 'PVS_REPORT_DIR must be empty; choose a new directory for each run' >&2
    exit 1
fi
command -v pvs-studio-analyzer >/dev/null
command -v plog-converter >/dev/null

# Configure the license in the disposable container, never during image build.
: "${LICENSE_NAME:?Set LICENSE_NAME}"
: "${LICENSE_KEY:?Set LICENSE_KEY}"
pvs-studio-analyzer credentials "$LICENSE_NAME" "$LICENSE_KEY"
unset LICENSE_NAME LICENSE_KEY

cd "$nginx"
configure=./configure
[ -f "$configure" ] || configure=./auto/configure
"$configure" --with-threads --with-debug \
    --without-http_rewrite_module --without-http_auth_basic_module \
    "--add-module=$module"
# Use the module manifest and NGINX's generated rules, including their actual
# include paths and feature defines. Do not compile or link unrelated core code.
targets=$(perl "$module/t/pvs_targets.pl" "$module/config" objs/Makefile)
mapfile -t objects <<< "$targets"
# Force every module unit into the trace even on a previously built tree.
pvs-studio-analyzer trace -- make -f objs/Makefile -B -j"$jobs" "${objects[@]}"
# Keep diagnostics in this module, including its platform headers; NGINX core
# and generated registration sources are outside the module's analysis scope.
pvs-studio-analyzer analyze -j "$jobs" -e "$nginx" \
    -o "$reports/pvs-studio.log"
plog-converter -a 'GA:1,2' -t tasklist \
    -o "$reports/pvs-studio.tasks" "$reports/pvs-studio.log"
# Apply the guide's two-step path conversion so /src container paths resolve
# to files in the GitHub checkout, rather than to files on the runner host.
cd "$reports"
plog-converter -a 'GA:1,2' -t json -n relative -R toRelative -r "$module" pvs-studio.log
plog-converter -a 'GA:1,2' -t sarif -n pvs-studio -r file:// relative.json
test -s "$reports/pvs-studio.sarif"
cat "$reports/pvs-studio.tasks"
