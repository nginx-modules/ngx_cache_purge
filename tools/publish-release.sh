#!/usr/bin/env bash
set -euo pipefail
: "${RELEASE_TAG:?Set the existing release tag}"
if [[ ! "$RELEASE_TAG" =~ ^4\.[0-9]+\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
    echo "Invalid RELEASE_TAG: expected 4.x.y[-prerelease] without a v prefix" >&2
    exit 1
fi
# Validate the complete matrix before creating or changing any release.
python3 - "$RELEASE_TAG" <<'PY'
import pathlib, sys
root = pathlib.Path('dist')
expected = {f'ngx_cache_purge-{sys.argv[1]}-nginx-{version}-linux-{arch}.{ext}'
            for version in ('1.30.6', '1.31.7')
            for arch in ('x86_64', 'aarch64') for ext in ('tar.gz', 'zip')}
actual = {p.name for p in root.iterdir() if p.name != 'SHA256SUMS'}
if actual != expected:
    raise SystemExit(f'Unexpected release assets; missing={expected-actual}, extra={actual-expected}')
PY
(cd dist && sha256sum ./*.tar.gz ./*.zip > SHA256SUMS)
if state=$(gh release view "$RELEASE_TAG" --json isDraft --jq .isDraft); then
    if [[ "$state" != true ]]; then
        echo 'Release is already published; assets will not be modified.' >&2
        exit 1
    fi
else
    prerelease=()
    if [[ "$RELEASE_TAG" == *-* ]]; then prerelease=(--prerelease); fi
    gh release create "$RELEASE_TAG" --verify-tag --draft --generate-notes \
        --title "ngx_cache_purge $RELEASE_TAG" "${prerelease[@]}"
fi
# Replacement is permitted only while the release remains a draft.
gh release upload "$RELEASE_TAG" dist/*.tar.gz dist/*.zip dist/SHA256SUMS --clobber
verify=$(mktemp -d)
trap 'rm -rf "$verify"' EXIT
gh release download "$RELEASE_TAG" --dir "$verify"
# Do not publish stale/extra assets left by another draft upload.
python3 - "$verify" <<'PYTHON'
import pathlib, sys
local = {p.name for p in pathlib.Path('dist').iterdir()}
remote = {p.name for p in pathlib.Path(sys.argv[1]).iterdir()}
if local != remote:
    raise SystemExit('Draft asset set differs from the verified build output')
PYTHON
cmp dist/SHA256SUMS "$verify/SHA256SUMS"
manifest="$(pwd)/dist/SHA256SUMS"
(cd "$verify" && sha256sum --check "$manifest")
gh release edit "$RELEASE_TAG" --draft=false
