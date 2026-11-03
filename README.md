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
- [Usage](#usage)
- [Configuration examples](docs/EXAMPLES.md)
- [REFRESH reference](docs/REFRESH.md)
- [Security](#security)
- [Migration](#migration)
- [Documentation](#documentation)
- [See also](#see-also)
- [Sponsors](#sponsors)
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
- **Status metrics** — per-cache queue, deletion and HIT/MISS statistics, optionally split by traffic class.
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

<a id="choosing-a-mode"></a>

For a comparison of PURGE, background cleanup, generation keys and REFRESH,
see [choosing a cache operation](docs/EXAMPLES.md#choosing-a-mode).

<a id="partial-key-purge"></a>

A trailing `*` selects matching cache keys. See
[partial-key PURGE](docs/EXAMPLES.md#partial-key-purge) for key-size limits,
case sensitivity and selector rules.

<a id="generation-invalidation"></a>

Generation keys prevent new requests from reaching invalidated entries,
including stale files written by older in-flight requests. See
[generation-key operation](docs/OPERATIONS.md#generation-invalidation) for
namespace persistence and cleanup constraints.

<a id="conditional-refresh"></a>

REFRESH checks cached proxy content against the origin without refilling
or extending its TTL. See [REFRESH semantics and limits](docs/REFRESH.md).

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

<a id="sponsors"></a>

## Sponsors

We thank the following organizations for providing tools and services free of charge to support the development and maintenance of `ngx_cache_purge` as an open-source project:
- [PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - static code analyzer for Enterprise (C, C++, C#, Go, and Java) and Web (JS and TS) development.

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
