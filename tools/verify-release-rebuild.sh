#!/usr/bin/env bash
set -euo pipefail
rebuild=$(mktemp -d)
trap 'rm -rf "$rebuild"' EXIT
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
    printf '### Repeat-build check: NGINX %s / %s\n\n' "$NGINX_VERSION" "$(uname -m)" >> "$GITHUB_STEP_SUMMARY"
    printf 'Two clean builds in different directories, on the same runner and toolchain.\n\n' >> "$GITHUB_STEP_SUMMARY"
fi
RELEASE_OUTPUT_DIR="$rebuild" bash tools/build-release-module.sh
python3 - "$rebuild" <<'PY'
import hashlib, os, pathlib, sys
rebuild = pathlib.Path(sys.argv[1])
# Other matrix versions may be present during local verification.
expected = sorted(rebuild.iterdir())
if len(expected) != 2:
    raise SystemExit('Expected exactly two rebuilt archives')
rows = []
passed = True
for second in expected:
    first = pathlib.Path('dist') / second.name
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    original = digest(first)
    match = original == digest(second)
    passed &= match
    rows.append(f'| `{second.name}` | {"PASS" if match else "FAIL"} | `{original}` |\n')
report = '| Archive | Identical bytes | SHA-256 |\n|---|---|---|\n' + ''.join(rows)
print(report)
if os.environ.get('GITHUB_STEP_SUMMARY'):
    with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as summary:
        summary.write(report)
        summary.write('\nThis checks repeatability in the recorded environment, not independent reproduction on another runner.\n')
if not passed:
    raise SystemExit('Rebuild differs: release publication blocked')
PY
