# ngx_cache_purge

An NGINX module for removing entries from `FastCGI`, `proxy`, `SCGI` and `uWSGI` caches. In v4, generation keys can stop new requests from using old entries before their files are deleted. REFRESH can ask a proxy origin whether cached content has changed. Both features require explicit configuration. Existing PURGE syntax remains available, but HTTP responses change; see [migration](#migration).

[Português (Brasil)](docs/i18n/README.pt-BR.md) · [Español](docs/i18n/README.es.md) · [Русский](docs/i18n/README.ru.md) · [Français](docs/i18n/README.fr.md) · [한국어](docs/i18n/README.ko.md) · [Türkçe](docs/i18n/README.tr.md) · [Bahasa Indonesia](docs/i18n/README.id.md) · [简体中文](docs/i18n/README.zh-CN.md) · [Deutsch](docs/i18n/README.de.md) · [日本語](docs/i18n/README.ja.md) · [Polski](docs/i18n/README.pl.md) · [Tiếng Việt](docs/i18n/README.vi.md) · [Українська](docs/i18n/README.uk.md) · [فارسی](docs/i18n/README.fa.md)

---

<a id="contents"></a>

## Contents

- [Features](#features)
- [Compatibility](#compatibility)
- [Distribution packages](#distribution-packages)
- [Installation](#installation)
- [Choose the behavior you need](#choosing-a-mode)
- [Usage](#usage)
- [Partial key purge](#partial-key-purge)
- [Stop serving old entries with generation keys](#generation-invalidation)
- [Check cached content against the origin (REFRESH)](#conditional-refresh)
- [Security](#security)
- [Migration](#migration)
- [Documentation](#documentation)
- [See also](#see-also)
- [License](#license)

<a id="features"></a>

## Features

- **Inline purge** — dedicated HTTP method (`PURGE`) with IP-based access control.
- **Separate purge location** — `proxy_cache_purge zone key` handles captured
  keys without requiring `proxy_pass` in the purge location.
- **Partial-key purge** — a trailing `*` selects matching cache entries.
- **`purge_all`** — purge every entry in a cache zone with one request.
- **Background queue** — defer filesystem walks to bounded event-loop steps or
  an optional NGINX thread pool, with configurable batching and throttling.
- **Vary-aware purge** — remove cached variants for an exact key, including
  gzip and `Vary` variants.
- **Response formats** — `json` (default), `html`, `xml`, `text` and `none`.
- **Named pools** — select a pool with incremental execution when threads are
  unavailable or on Windows.
- **Active-work deduplication** — coalesce duplicate purge requests during a
  walk with at most one pending replay.
- **Status metrics** — per-cache queue, deletion and HIT/MISS statistics.
- **Generation keys** — invalidate exact keys, prefixes or namespaces without
  synchronous deletion; late writes to old generations remain unreachable.
- **REFRESH** — check cached proxy content against the origin without
  prewarming it. See [REFRESH](#conditional-refresh).

<a id="compatibility"></a>

## Compatibility

Use a maintained NGINX release for production. POSIX/UNIX is the primary target; Windows is experimental. Threads are optional: without `--with-threads`, background walks run in small event-loop steps. See the [tested versions and platforms](t/TESTING.md#compatibility-matrix).

<a id="distribution-packages"></a>

## Distribution packages

Several distributions and ports projects package this fork, but their builds
may use older versions rather than v4. Check the package version and NGINX ABI
before installing. See the [distribution packages and sources](docs/DISTRIBUTIONS.md).

<a id="installation"></a>

## Installation

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Dynamic module:

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Use matching NGINX sources; `--with-compat` does not provide compatibility across arbitrary NGINX versions or C libraries. See [dynamic modules and release archives](docs/BINARY_MODULES.md).

<a id="choosing-a-mode"></a>

## Choose the behavior you need

| Your goal | Configuration | What the response means |
|---|---|---|
| Delete known stale entries and wait for the deletion attempt | PURGE with the background queue off | `200 purged` or `200 no_match`; errors use 4xx/5xx |
| Schedule a large prefix or full-cache cleanup | PURGE with `cache_purge_background_queue on` | `202 accepted`; files may still exist and be served while cleanup is pending |
| Stop new requests from hitting old content, including late writes | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; new requests use a new key generation; disk cleanup follows |
| Check whether cached proxy content changed at the origin | Generation keys + `proxy_cache_refresh` + an internal profile | `200` with per-entry results and continuation; it does not prewarm the cache |

These settings combine: generation invalidation also schedules cleanup. The background-queue switch alone does not protect against late writes. Read [generation-key example](docs/EXAMPLES.md#generation-and-refresh-example) for the v4 setup, [sample configurations](docs/EXAMPLES.md#sample-configurations) for ordinary PURGE, and [HTTP results](docs/HTTP_RESPONSES.md) before writing a client.

<a id="usage"></a>

## Usage

Start with the [inline PURGE example](docs/EXAMPLES.md#basic--inline-purge-method),
then choose the behavior that matches your cache invalidation requirements.
The [configuration examples](docs/EXAMPLES.md#sample-configurations) cover
separate purge locations, background cleanup, full-cache purge, response
formats and Vary-aware removal.

<a id="generation-and-refresh-example"></a>

For v4 generation-key invalidation and conditional REFRESH, follow the
[complete product catalog example](docs/EXAMPLES.md#generation-and-refresh-example).


<a id="partial-key-purge"></a>

## Partial key purge

Append `*` to the **expanded cache key** for a case-insensitive prefix match. Exact-key and Vary matching remain case-sensitive. A physical prefix may contain up to 511 bytes, excluding the trailing `*`. A longer prefix is logged once and returns `400 invalid_selector` without walking files.

With `cache_purge_key`, matching uses the logical expression and accepts at most 8096 bytes including a trailing `*`.

For a key expression ending in `$uri`, this request selects a URI prefix:

```http
PURGE /images/header*
```

The asterisk must be the last byte of the evaluated key, not merely the last byte of the URI path. Account for any query-string or variable suffix in `proxy_cache_key`. A key consisting only of `*` walks every eligible entry.

<a id="generation-invalidation"></a>

## Stop serving old entries with generation keys

Use generation keys when newly arriving requests must stop using stale content as soon as PURGE completes. A request already fetching from the origin may finish later, but its old cache write cannot become a HIT for new requests. Physical files are deleted separately.

A namespace groups keys that share invalidation rules. Keep its state directory with the cache across restarts. Sharing a namespace across cache paths shares invalidation, while cleanup only targets the addressed path. See [operating notes](docs/OPERATIONS.md) before deployment.

<a id="conditional-refresh"></a>

## Check cached content against the origin (REFRESH)

Use REFRESH when you need to ask whether cached content changed. It sends HEAD to the same origin representation using the cached ETag and Last-Modified values. If you already know a product page is outdated, use PURGE.

REFRESH does not download a replacement, prewarm an empty cache or extend TTL. Read each entry result. If `complete` is false, repeat the same selector with the returned `next_cursor` in `X-Cache-Purge-Cursor`. See the [response contract and limits](docs/REFRESH.md).

| Origin response or entry state | What happens |
|---|---|
| `304` | Keep the current entry: `kept`. Its TTL is not extended. |
| `200`, `404`, `410` | Invalidate the current generation: `invalidated`. The next ordinary GET refills the cache if the origin response is cacheable. |
| Another status | Keep the entry: `upstream_status`. Inspect `upstream_status` for the origin's code. |
| No origin response | Keep the entry: `transport_error`. |
| Vary metadata | Skip the probe: `skipped_vary`; the original request headers are not available. |
| Invalid profile or key-to-URI mapping | Keep the entry: `skipped_profile`. Check profile diagnostics and the URI mapping. |
| Another PURGE changed the generation meanwhile | Leave that newer generation alone: `raced_replaced`. |
| Invalidation/allocation failure | Keep the entry: `invalidation_error`. |
| Selected work did not finish | Count it in `undispatched`; continuation does not move past it. |

<a id="security"></a>

## Security

- Restrict purge endpoints with `allow` / `deny` directives or an upstream authentication layer. Never expose them publicly.
- Set `cache_purge_queue_size` to a value appropriate for your traffic to limit shared memory consumption under request floods.
- Consider pairing purge locations with `limit_req` to rate-limit purge requests independently of normal traffic.

<a id="migration"></a>

## Migration

v4 defaults to JSON and returns `200 no_match` for a missing entry. `cache_purge_legacy_status` only warns and no longer changes responses. Update clients and perform a full restart when upgrading from a different shared-memory layout. Generation keys are opt-in and change cache filenames. See [migration details](docs/MIGRATION.md).

<a id="documentation"></a>

## Documentation

- [Directive reference](docs/DIRECTIVES.md)
- [More configuration examples](docs/EXAMPLES.md)
- [Operations and troubleshooting](docs/OPERATIONS.md)
- [HTTP results and REFRESH limits](docs/REFRESH.md)
- [Migration to v4](docs/MIGRATION.md)
- [Implementation architecture](docs/ARCHITECTURE.md)
- [Development and tests](t/TESTING.md)

<a id="see-also"></a>

## See also

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)

<a id="license"></a>

## License

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denys Denysov

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```
