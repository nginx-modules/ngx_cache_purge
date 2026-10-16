# Dynamic modules

Build a module against the exact NGINX source version used by the target server:

```sh
nginx -V
cd /path/to/matching-nginx-source
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Copy `objs/ngx_http_cache_purge_module.so` into the server's module directory. Add `load_module` in the main configuration context, before `events` and `http`, then check the configuration before reloading:

```nginx
load_module modules/ngx_http_cache_purge_module.so;
```

```sh
nginx -t
```

Use the target server's relevant configure options shown by `nginx -V`. `--with-compat` is available from NGINX 1.11.5; older dynamic-module targets need their original matching build options. The module's static-build minimum is unchanged.

## What compatibility means

`--with-compat` accommodates certain differences in NGINX build options. It does not remove the NGINX version check or guarantee compatibility across CPU architectures, C libraries, distribution patches or arbitrary NGINX derivatives. A binary for NGINX 1.31.7 is not a binary for every 1.31.x release. Always run `nginx -t` on the actual target installation.

NGINX Open Source is the open-source server. NGINX Plus is the commercial server offered within F5 NGINX One. For Plus, identify the underlying NGINX version with `nginx -v` and follow [F5's third-party module build instructions](https://www.f5.com/company/blog/nginx/compiling-dynamic-modules-nginx-plus). These release assets are built and load-tested with NGINX Open Source; they are not certified or tested against NGINX Plus. Do not infer Plus compatibility from the name of a stable or mainline branch.

## Release archives

The release matrix pins one current stable and one current mainline version, each built natively on Linux x86_64 and aarch64. It does not download a moving `latest` version during the release. Review the pins before each module release; users of other NGINX versions should build from matching sources.

Example archive names:

- `ngx_cache_purge-v4.0.0-nginx-1.31.7-linux-x86_64.tar.gz`
- `ngx_cache_purge-v4.0.0-nginx-1.31.7-linux-aarch64.zip`

Both formats contain the same module, license, instructions, `SHA256SUMS` for the module and `BUILD-INFO.txt`. Build information records the commit, source archive hash, configure options, compiler, build OS, glibc and ELF dependencies. The build uses pinned Ubuntu 24.04 runners and glibc. The OS version is omitted from the archive name, not from the compatibility information. These are not Alpine/musl binaries; musl would require a separate build and clearly distinguished asset name.

Download the release-level `SHA256SUMS` alongside the archives to check their hashes. These hashes detect corruption; they are not an independent signature. Inspect the recorded requirements before installing on a different distribution.

## Publishing an immutable release

Push an existing `v4.*` tag to start CI. After the required CI jobs pass, four native jobs compile NGINX and the dynamic module, check that NGINX loads the module and accepts a purge configuration, and produce eight archives. The publishing job requires the complete set, creates or resumes a draft, uploads the archives and checksums, downloads them again and verifies their hashes before publishing.

Do not publish the release manually before this workflow finishes. With [immutable releases](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository), assets must be attached before publication. A failed upload leaves a draft that can be retried. A rerun refuses to modify an already published release. Unexpected assets in an existing draft block publication rather than silently becoming part of the release.

The tag must be pushed by a user or another credential that triggers workflows. A tag created using a workflow's default `GITHUB_TOKEN` does not trigger another push workflow. Creating a release in the GitHub UI is not a substitute for this tag-push workflow.


## Repeatable builds and provenance

Each release matrix job runs two clean builds in different temporary directories and compares the SHA-256 hashes of both archive formats. A mismatch fails the job and blocks publication. The `Rebuild check` job and its summary display the result and archive hashes.

The build uses the commit timestamp as `SOURCE_DATE_EPOCH`, normalizes compiler source paths, and fixes archive entry order, timestamps and ownership. `BUILD-INFO.txt` records compiler, linker, packaging tools, installed package versions and source hashes. To repeat the check locally in the same compatible Linux build environment, run from the module checkout:

```sh
export NGINX_VERSION=1.31.7 MODULE_VERSION=v4.0.0
bash tools/build-release-module.sh
bash tools/verify-release-rebuild.sh
```

This is a same-environment repeat-build check. Hosted runner images and APT repositories can change; version labels and recorded package versions alone do not freeze that environment. Independent reproduction later requires retaining a build environment pinned by image digest and dependency snapshots. The current check does not claim that guarantee.

[Build provenance attestations](https://docs.github.com/en/actions/concepts/security/artifact-attestations) identify the source commit and GitHub Actions workflow that produced the archives. Verify a downloaded archive with:

```sh
gh attestation verify ngx_cache_purge-v4.0.0-nginx-1.31.7-linux-x86_64.tar.gz \
    -R nginx-modules/ngx_cache_purge
```

[Release immutability](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases) prevents changes to the published tag and assets and provides a release attestation. Neither immutability nor provenance proves byte-for-byte reproducibility. Attestations are generated after the rebuild check and are not embedded in the compared archives.
