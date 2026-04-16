# ngx_cache_purge

`ngx_cache_purge` is an `nginx` module that adds cache purge support for `FastCGI`, `proxy`, `SCGI`, and `uWSGI` caches. A purge operation removes or expires cached content that matches the cache key, wildcard key, or configured cache tags for the request.

_This module is not distributed with the NGINX source. See [Installation Instructions](#installation-instructions)._

## Status

This is a fork of the [`ngx_cache_purge` module](https://github.com/nginx-modules/ngx_cache_purge) to add support for soft purgaging and cache tags (also known as surrogate keys).

## Quick Start

`ngx_cache_purge` supports multiple purge styles depending on how you want to address cached content:

- exact URI purge
- wildcard URI purge using a trailing `*`
- cache-tag purge
- surrogate-key purge

For most users, the simplest starting point is a cached location plus a `PURGE` method restricted to trusted clients.

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;

    server {
        listen 8080;

        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
            proxy_cache_purge  PURGE from 127.0.0.1;
        }
    }
}
```

That allows requests such as:

```bash
curl -i -X PURGE 'http://127.0.0.1:8080/path?query=1'
```

If the configured cache key ends with `$uri`, you can also purge by wildcard URI using a trailing `*`:

```bash
curl -i -X PURGE 'http://127.0.0.1:8080/articles/2026/*'
```

If you want cache-tag purging, enable an index backend and watch the cache directory:

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;
    cache_tag_index  sqlite /tmp/ngx_cache_purge_tags.sqlite;

    server {
        location /tagged/ {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
            proxy_cache_purge  PURGE soft from 127.0.0.1;
            cache_purge_mode_header X-Purge-Mode;
            cache_tag_watch    on;
        }
    }
}
```

That unlocks tag-based requests such as:

```bash
curl -i -X PURGE -H 'Cache-Tag: article-42, group-a' 'http://127.0.0.1:8080/tagged/item'
curl -i -X PURGE -H 'Cache-Tag: article-42, group-a' -H 'X-Purge-Mode: soft' 'http://127.0.0.1:8080/tagged/item'
```

Or surrogate-key requests such as:

```bash
curl -i -X PURGE -H 'Surrogate-Key: article-42 group-a' 'http://127.0.0.1:8080/tagged/item'
curl -i -X PURGE -H 'Surrogate-Key: article-42 group-a' -H 'X-Purge-Mode: soft' 'http://127.0.0.1:8080/tagged/item'
```

## Installation Instructions

You need to build NGINX with this repository as an extra module via `--add-module` or `--add-dynamic-module`; it is not bundled with upstream NGINX.

For most users, the recommended installation path is to build a dynamic module against the exact NGINX version already installed on the target system.

### Recommended: build a dynamic module for your installed NGINX version

- Check the target version with `nginx -v`.
- Download the matching NGINX source release.
- Build this repository as a dynamic module against that exact source tree.
- Copy the resulting `.so` into your NGINX modules directory and load it with `load_module`.

For example, if `nginx -v` reports `nginx/1.28.1`:

```bash
cd ~/build/nginx-cache-purge
wget https://nginx.org/download/nginx-1.28.1.tar.gz
tar xf nginx-1.28.1.tar.gz
cd nginx-1.28.1

./configure \
    --with-compat \
    --with-ld-opt="-lsqlite3" \
    --add-dynamic-module=../ngx_cache_purge

make modules
```

This produces `objs/ngx_http_cache_purge_module.so`, which you can then copy into your nginx modules directory and load with `load_module`.

### Alternative: build NGINX from source with this module

If you are building your own NGINX binary from source, point `./configure` at this repository with `--add-module` for a static build or `--add-dynamic-module` for a dynamic build.

```bash
./configure \
    --with-debug \
    --with-http_ssl_module \
    --add-module=/path/to/ngx_cache_purge
make
make install
```

For a dynamic module build in this workflow, replace `--add-module` with `--add-dynamic-module` and use `make modules`.

The repository `config` script links against `sqlite3`, so your build environment must provide the SQLite development library. Redis support uses the module's built-in RESP client and does not add another native dependency. The resulting dynamic module still depends on the system `libsqlite3` when SQLite support is compiled in.

If you want the included containerized build environment, tests, or the manual validation setup, see [Development](#development).

## Configuration Reference

### Same-location syntax

#### `fastcgi_cache_purge`

- **syntax**: `fastcgi_cache_purge on|off|<method> [soft] [purge_all] [from all|<ip> [.. <ip>]]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `FastCGI` cache.

#### `proxy_cache_purge`

- **syntax**: `proxy_cache_purge on|off|<method> [soft] [purge_all] [from all|<ip> [.. <ip>]]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `proxy` cache.

#### `scgi_cache_purge`

- **syntax**: `scgi_cache_purge on|off|<method> [soft] [purge_all] [from all|<ip> [.. <ip>]]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `SCGI` cache.

#### `uwsgi_cache_purge`

- **syntax**: `uwsgi_cache_purge on|off|<method> [soft] [purge_all] [from all|<ip> [.. <ip>]]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `uWSGI` cache.

### Separate-location syntax

#### `fastcgi_cache_purge`

- **syntax**: `fastcgi_cache_purge zone_name key [soft]`
- **default**: `none`
- **context**: `location`

Set the cache zone and key used for purging selected pages from `FastCGI` cache.

#### `proxy_cache_purge`

- **syntax**: `proxy_cache_purge zone_name key [soft]`
- **default**: `none`
- **context**: `location`

Set the cache zone and key used for purging selected pages from `proxy` cache.

#### `scgi_cache_purge`

- **syntax**: `scgi_cache_purge zone_name key [soft]`
- **default**: `none`
- **context**: `location`

Set the cache zone and key used for purging selected pages from `SCGI` cache.

#### `uwsgi_cache_purge`

- **syntax**: `uwsgi_cache_purge zone_name key [soft]`
- **default**: `none`
- **context**: `location`

Set the cache zone and key used for purging selected pages from `uWSGI` cache.

### Optional directives

#### `cache_purge_response_type`

- **syntax**: `cache_purge_response_type html|json|xml|text`
- **default**: `html`
- **context**: `http`, `server`, `location`

Set the response type returned after a purge.

#### `cache_purge_mode_header`

- **syntax**: `cache_purge_mode_header <header>`
- **default**: `none`
- **context**: `http`, `server`, `location`

Enable request-time soft/hard purge override using the named request header. When unset, purge mode is controlled only by the configured `soft` flag.

If configured:

- header value `soft`, `true`, or `1` forces a soft purge
- any other present value forces a hard purge
- if the header is absent, the configured purge mode is used
- `purge_all` ignores this override and keeps its configured behavior

#### `cache_tag_index`

- **syntax**: `cache_tag_index sqlite <path>` or `cache_tag_index redis <endpoint> [db=<n>] [password=<secret>]`
- **default**: `none`
- **context**: `http`

Enable cache-tag indexing backed by SQLite or Redis. This feature is currently Linux-only. SQLite requires a writable database path. Redis currently supports a single instance over `host:port` or `unix:/path`, with optional `db=<n>` and `password=<secret>`, but no TLS, Sentinel, or Cluster support.

#### `cache_tag_headers`

- **syntax**: `cache_tag_headers <header> [header ...]`
- **default**: `Surrogate-Key Cache-Tag`
- **context**: `http`, `server`, `location`

Set the request and cached-response headers used for cache-tag extraction and tag purge matching.

All watched locations that share the same cache zone must use the same `cache_tag_headers` list.

#### `cache_tag_watch`

- **syntax**: `cache_tag_watch on|off`
- **default**: `off`
- **context**: `http`, `server`, `location`

Enable cache-tag indexing for the cache used by the current purge-enabled location. When enabled, the module watches the cache directory, indexes tags found in cached response headers, and allows tag-based `PURGE` requests.

For hard tag purges, matching cache files are removed immediately and the corresponding SQLite index deletes are handed off asynchronously to the owner worker. A successful purge response means all required index deletes were accepted for processing; if that handoff cannot be accepted, the request fails with `500`.

## Partial Keys

Sometimes it is not possible to pass the exact cache key to purge a page. For example, parts of the key may depend on cookies or query parameters. You can specify a partial key by adding an asterisk at the end of the URL.

```bash
curl -X PURGE /page*
```

The asterisk must be the last character of the key, so you must put the `$uri` variable at the end of the configured cache key.

## Soft Purge

By default, soft purge behavior is still controlled by the configured `soft` parameter.

If `cache_purge_mode_header` is configured, exact-key, wildcard, and cache-tag / surrogate-key purges can override that mode per request. A value of `soft`, `true`, or `1` forces a soft purge; any other present value forces a hard purge.

The `soft` config parameter still controls `purge_all`, which does not honor `cache_purge_mode_header`.

- Exact-key soft purge marks the cached entry as expired, so the next request is handled as `EXPIRED` rather than a deletion-driven `MISS`.
- Wildcard soft purge applies the same expiration behavior to all matching keys.
- `purge_all` can also be combined with `soft` to expire every cached entry in a zone without removing the underlying cache files immediately.

For wildcard and `purge_all` soft purges, the module expires both the cache-file header on disk and the matching shared-memory cache node so the next lookup is treated as expired consistently.

## Cache Tags

The module can also purge cached objects by cache tag, similar to `Surrogate-Key` or `Cache-Tag` support in other reverse proxies.

When `cache_tag_index` and `cache_tag_watch` are enabled:

- cached response files are parsed for the headers listed in `cache_tag_headers`
- `Surrogate-Key` values are parsed as comma- or whitespace-delimited tags
- `Cache-Tag` values are parsed as comma- or whitespace-delimited tags
- the module stores a tag-to-cache-file index in SQLite or Redis
- on Linux, a worker-owned `inotify` watcher keeps the index up to date as cache files are created, replaced, or removed

To purge by tag, send a normal `PURGE` request and include one or more tag headers:

```bash
curl -i -X PURGE -H 'Surrogate-Key: article-42 group-a' 'http://127.0.0.1/tagged/item'
curl -i -X PURGE -H 'Cache-Tag: article-42, group-a' 'http://127.0.0.1/tagged/item'
curl -i -X PURGE -H 'Surrogate-Key: article-42 group-a' -H 'X-Purge-Mode: soft' 'http://127.0.0.1/tagged/item'
curl -i -X PURGE -H 'Cache-Tag: article-42, group-a' -H 'X-Purge-Mode: soft' 'http://127.0.0.1/tagged/item'
```

All supplied tags are matched with OR semantics. If any cached file is indexed under any supplied tag, it will be purged.

If a watched purge location receives a plain `PURGE` request without any of the configured tag headers, the module falls back to the normal key-based purge behavior for that location.

For tag-based purges, the configured `cache_purge_mode_header` can switch a request between soft and hard purge. Without that header, the configured purge mode is used.

Hard tag purges use asynchronous owner-worker handoff for backend index deletes. A `200` response means the delete work was accepted for processing, not necessarily already persisted yet.

Notes:

- Cache-tag support currently requires Linux.
- Supported tag index backends are SQLite and Redis.
- Redis support currently targets a single instance over TCP or a Unix socket, with optional password auth and database selection.
- The cache watcher keeps the index fresh during normal operation.
- A cold-start bootstrap fallback scans the configured cache tree if a tag purge arrives before a zone has been indexed.

## Configuration Examples

Use these as compact starting points after Quick Start.

### Same-location syntax

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;

    server {
        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
            proxy_cache_purge  PURGE from 127.0.0.1;
        }
    }
}
```

Use `soft` if you want matching entries to expire in place, or add `purge_all` if you want a purge request to target every cached entry in the zone.

### Separate-location syntax

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;

    server {
        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
        }

        location ~ /purge(/.*) {
            allow              127.0.0.1;
            deny               all;
            proxy_cache_purge  tmpcache "$1$is_args$args";
        }
    }
}
```

### Response types

Use `cache_purge_response_type` to switch between `html`, `json`, `xml`, and `text` responses in the scope where the purge response is generated.

### Cache tags

The minimal cache-tag setup is already shown in Quick Start. Use that pattern whenever you want to purge by `Cache-Tag` or `Surrogate-Key` headers.

## Known issues

- **`gzip_vary` and Vary-based cache variants** — When [`gzip_vary`](https://nginx.org/r/gzip_vary) (or `brotli_vary` / `zstd_vary`) is enabled, nginx stores a separate cache file for each `Accept-Encoding` variant of the same URL. A key-based purge uses `ngx_http_file_cache_open`, which resolves to the single variant whose `Vary` headers match the incoming purge request, leaving other variants in the cache. **Recommended fix:** use cache tags. Every cached file — including each encoding variant — is registered in the tag store independently at write time, so a tag-based purge removes all variants regardless of encoding. See [#20](https://github.com/nginx-modules/ngx_cache_purge/issues/20).

## Development

Use this section if you are hacking on the module, running the automated test suite, or validating behavior inside the included container.

The repository includes a containerized build environment with:

- Debian-based build tooling for NGINX modules
- downloaded NGINX source in `/opt/nginx-src/nginx-$NGINX_VERSION`
- `Test::Nginx` installed from `openresty/test-nginx`

### Development container

Use the included container for development, testing, and manual validation. It is not the primary installation path for matching a system-provided NGINX package.

```bash
make shell
make nginx-build
make nginx-version
```

### Common development commands

```bash
make format
make test
```

### Docker Validation Config

For manual validation inside the development container, the repository includes an example nginx configuration at `examples/docker-validation.conf`.

It defaults to SQLite for tag indexing and includes a commented Redis alternative:

```nginx
cache_tag_index  sqlite /tmp/ngx_cache_purge_demo_tags.sqlite;
# cache_tag_index  redis redis:6379 db=10;
```

It provides separate locations for these behaviors:

- exact-key soft purge (`/soft`)
- soft purge with `proxy_cache_use_stale` on upstream `500` (`/stale`)
- wildcard soft purge (`/wild`)
- `purge_all` soft purge (`/purge_all`)
- separate-location `zone key soft` syntax (`/separate` and `/purge_separate/...`)
- cache-tag soft purge by `Surrogate-Key` or `Cache-Tag` (`/tagged/...`)
- watched-location plain `PURGE` fallback (`/tagged/plain`)
- custom tag headers with an isolated cache zone (`/tagged_custom`)

Start it inside the container after building nginx:

```bash
make shell
make nginx-build
rm -rf /tmp/ngx_cache_purge_demo_* /tmp/ngx_cache_purge_temp /tmp/ngx_cache_purge_demo_tags.sqlite
mkdir -p /tmp/ngx_cache_purge_temp /tmp/logs
/opt/nginx/sbin/nginx -p /tmp -c /workspace/examples/docker-validation.conf
```

For Redis-backed validation, start the sidecar first, switch `cache_tag_index` in the example config to the commented Redis line, and clear the selected database before starting nginx:

```bash
docker compose up -d redis
make shell
make nginx-build
redis-cli -h redis -p 6379 -n 10 FLUSHDB
rm -rf /tmp/ngx_cache_purge_demo_* /tmp/ngx_cache_purge_temp /tmp/ngx_cache_purge_demo_tags.sqlite
mkdir -p /tmp/ngx_cache_purge_temp /tmp/logs
/opt/nginx/sbin/nginx -p /tmp -c /workspace/examples/docker-validation.conf
```

Exact-key soft purge flow:

```bash
curl -i 'http://127.0.0.1:8080/soft/item?t=soft'
curl -i 'http://127.0.0.1:8080/soft/item?t=soft'
curl -i -X PURGE 'http://127.0.0.1:8080/soft/item?t=soft'
curl -i 'http://127.0.0.1:8080/soft/item?t=soft'
curl -i 'http://127.0.0.1:8080/soft/item?t=soft'
```

Expected `X-Cache-Status` values are `MISS`, `HIT`, purge `200`, `EXPIRED`, then `HIT`.

`proxy_cache_use_stale` flow:

```bash
curl -i 'http://127.0.0.1:8080/stale/item?t=demo'
curl -i -X PURGE 'http://127.0.0.1:8080/stale/item?t=demo'
curl -i -H 'X-Origin-Fail: 1' 'http://127.0.0.1:8080/stale/item?t=demo'
```

The final request should return cached content with `X-Cache-Status: STALE` because the expired entry exists but the origin is forced to return `500`.

Wildcard soft purge flow:

```bash
curl -i 'http://127.0.0.1:8080/wild/pass-one'
curl -i 'http://127.0.0.1:8080/wild/pass-two'
curl -i 'http://127.0.0.1:8080/wild/other'
curl -i -X PURGE 'http://127.0.0.1:8080/wild/pass*'
curl -i 'http://127.0.0.1:8080/wild/pass-one'
curl -i 'http://127.0.0.1:8080/wild/pass-two'
curl -i 'http://127.0.0.1:8080/wild/other'
```

The two `pass*` entries should come back as `EXPIRED`, while `/wild/other` should remain `HIT`.

`purge_all` soft purge flow:

```bash
curl -i 'http://127.0.0.1:8080/purge_all/one?t=1'
curl -i 'http://127.0.0.1:8080/purge_all/two?t=2'
curl -i -X PURGE 'http://127.0.0.1:8080/purge_all/anything'
curl -i 'http://127.0.0.1:8080/purge_all/one?t=1'
curl -i 'http://127.0.0.1:8080/purge_all/two?t=2'
```

The post-purge requests should return `X-Cache-Status: EXPIRED`.

Separate-location soft purge flow:

```bash
curl -i 'http://127.0.0.1:8080/separate/item?t=sep'
curl -i -X PURGE 'http://127.0.0.1:8080/purge_separate/separate/item?t=sep'
curl -i 'http://127.0.0.1:8080/separate/item?t=sep'
```

The final request should return `X-Cache-Status: EXPIRED`.

Cache-tag soft purge flow using `Surrogate-Key`:

```bash
curl -i 'http://127.0.0.1:8080/tagged/a'
curl -i 'http://127.0.0.1:8080/tagged/b'
curl -i 'http://127.0.0.1:8080/tagged/c'
curl -i -X PURGE -H 'Surrogate-Key: group-one' 'http://127.0.0.1:8080/tagged/a'
curl -i 'http://127.0.0.1:8080/tagged/a'
curl -i 'http://127.0.0.1:8080/tagged/b'
curl -i 'http://127.0.0.1:8080/tagged/c'
```

The two `group-one` entries should come back as `EXPIRED`, while `/tagged/c` should remain `HIT`.

Cache-tag soft purge flow using `Cache-Tag`:

```bash
curl -i 'http://127.0.0.1:8080/tagged/a'
curl -i 'http://127.0.0.1:8080/tagged/b'
curl -i 'http://127.0.0.1:8080/tagged/c'
curl -i -X PURGE -H 'Cache-Tag: shared' 'http://127.0.0.1:8080/tagged/a'
curl -i 'http://127.0.0.1:8080/tagged/a'
curl -i 'http://127.0.0.1:8080/tagged/b'
curl -i 'http://127.0.0.1:8080/tagged/c'
```

The two `shared` entries should come back as `EXPIRED`, while `/tagged/c` should remain `HIT`.

Redis-specific validation flows after switching the example config to `cache_tag_index redis redis:6379 db=10`:

Watched-location plain `PURGE` fallback:

```bash
curl -i 'http://127.0.0.1:8080/tagged/plain'
curl -i -X PURGE 'http://127.0.0.1:8080/tagged/plain'
curl -i 'http://127.0.0.1:8080/tagged/plain'
```

The final request should return `X-Cache-Status: EXPIRED`, showing that a watched location still falls back to key-based soft purge when no tag headers are supplied.

Redis hard tag purge via `cache_purge_mode_header` override:

```bash
curl -i 'http://127.0.0.1:8080/tagged/a?t=redis-hard'
curl -i -X PURGE -H 'Cache-Tag: alpha' -H 'X-Purge-Mode: hard' 'http://127.0.0.1:8080/tagged/a?t=redis-hard'
curl -i 'http://127.0.0.1:8080/tagged/a?t=redis-hard'
```

The final request should return `X-Cache-Status: MISS`, confirming that the Redis-backed tag purge deleted the cache file instead of expiring it in place.

Redis custom `cache_tag_headers` flow:

```bash
curl -i 'http://127.0.0.1:8080/tagged_custom'
curl -i -X PURGE -H 'Custom-Group: custom-alpha' 'http://127.0.0.1:8080/tagged_custom'
curl -i 'http://127.0.0.1:8080/tagged_custom'
```

The final request should return `X-Cache-Status: EXPIRED`, confirming that both cached-response indexing and purge matching use `Edge-Tag` and `Custom-Group` for that isolated Redis-backed zone.

Stop the validation nginx instance with:

```bash
kill "$(cat /tmp/ngx-cache-purge-validation.pid)"
```

## License

```text
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
All rights reserved.

This project was fully funded by yo.se.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## See Also

- [ngx_slowfs_cache](http://github.com/FRiCKLE/ngx_slowfs_cache)
- http://nginx.org/en/docs/http/ngx_http_fastcgi_module.html#purger
- http://nginx.org/en/docs/http/ngx_http_fastcgi_module.html#fastcgi_cache_purge
- https://github.com/wandenberg/nginx-selective-cache-purge-module
- https://github.com/wandenberg/nginx-sorted-querystring-module
- https://github.com/ledgetech/ledge
- [Faking Surrogate Cache-Keys for Nginx Plus](https://www.innoq.com/en/blog/faking-surrogate-cache-keys-for-nginx-plus/) ([gist](https://gist.github.com/titpetric/2f142e89eaa0f36ba4e4383b16d61474))
- [Delete NGINX cached md5 items with a PURGE with wildcard support](https://gist.github.com/nosun/0cfb58d3164f829e2f027fd37b338ede)
