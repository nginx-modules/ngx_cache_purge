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
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }

    server {
        listen 8080;

        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
            proxy_cache_purge  $purge_method;
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
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    cache_tag_index  sqlite /tmp/ngx_cache_purge_tags.sqlite;

    server {
        location /tagged/ {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
            proxy_cache_purge  $purge_method soft;
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
    --with-threads \
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
    --with-threads \
    --with-http_ssl_module \
    --add-module=/path/to/ngx_cache_purge
make
make install
```

For a dynamic module build in this workflow, replace `--add-module` with `--add-dynamic-module` and use `make modules`.

The repository `config` script links against `sqlite3`, so your build environment must provide the SQLite development library. Redis support uses the module's built-in RESP client and does not add another native dependency. The resulting dynamic module still depends on the system `libsqlite3` when SQLite support is compiled in.

`--with-threads` enables nginx's thread pool support. When present, the module offloads two blocking operations to a worker thread: the startup cache-tree bootstrap (tag index population) and wildcard/partial-key purge scans. Without `--with-threads` these operations run synchronously in the event loop.

If you want the included containerized build environment, tests, or the manual validation setup, see [Development](#development).

## Configuration Reference

#### `fastcgi_cache_purge`

- **syntax**: `fastcgi_cache_purge string ... [soft] [purge_all]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `FastCGI` cache. Purge is enabled when at least one condition value is non-empty and not equal to `"0"`. This matches nginx core purge semantics, with `soft` and `purge_all` kept as module-specific trailing flags.

#### `proxy_cache_purge`

- **syntax**: `proxy_cache_purge string ... [soft] [purge_all]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `proxy` cache. Purge is enabled when at least one condition value is non-empty and not equal to `"0"`.

#### `scgi_cache_purge`

- **syntax**: `scgi_cache_purge string ... [soft] [purge_all]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `SCGI` cache. Purge is enabled when at least one condition value is non-empty and not equal to `"0"`.

#### `uwsgi_cache_purge`

- **syntax**: `uwsgi_cache_purge string ... [soft] [purge_all]`
- **default**: `none`
- **context**: `http`, `server`, `location`

Allow purging of selected pages from `uWSGI` cache. Purge is enabled when at least one condition value is non-empty and not equal to `"0"`.

For dedicated purge locations, configure the cache zone with `*_cache`, the purge key with `*_cache_key`, and then enable purging with one or more string conditions plus optional `soft` / `purge_all` flags.

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
- When built with `--with-threads`, the startup cache-tree bootstrap and wildcard purge scans run in an nginx thread pool, keeping the event loop unblocked. Without threads, both operations run synchronously.

## Cache Index Architecture

This section describes how the cache index works internally. It is not required reading for normal use, but is useful when diagnosing storage growth, planning capacity, or modifying the module.

### Overview

The cache index maps cache tags to the physical file paths of cached responses. When a `PURGE` request arrives with tag headers, the module looks up all paths associated with those tags in the index and purges each file. Without an index there would be no efficient way to find which files carry a given tag across a potentially large cache directory tree.

### Index population

The index is built and kept current through two mechanisms that work together:

**inotify watcher (Linux only).** When `cache_tag_watch on` is set, one worker process (the owner) opens an `inotify` watch on the cache directory tree. When other workers create or replace a cache file they enqueue a write operation into a shared-memory ring buffer. The owner worker drains this queue on a 10 ms timer and writes the tag associations to the index backend. Delete events are handled the same way.

**Cold-start bootstrap.** If a tag `PURGE` request arrives before a zone has been indexed — for example after a restart — the module scans the entire cache directory tree, reads the cached response headers from every file it finds, extracts tags, and writes all associations to the index before completing the purge. The result is recorded so subsequent requests skip the scan.

**Tag extraction.** Both paths use the same extraction logic. The module reads the configured header names (`Surrogate-Key` and `Cache-Tag` by default) from the binary nginx cache file header and splits the value on commas and whitespace. Duplicate tags within a single response are deduplicated. The hard limit is 1000 tags per cached file.

### Shared-memory write queue

Workers communicate with the index-owning worker through a fixed-size ring buffer allocated in shared memory (2 MB zone, capacity 256 entries). Each entry holds an operation type (replace or delete), the zone name, and the full cache file path. If the buffer is full the operation is dropped and a warning is logged; the cache file itself is still served and purged normally, but that file's index entry may become stale until the next inotify event corrects it.

### SQLite backend

The SQLite file uses WAL journal mode and `SYNCHRONOUS = NORMAL` to balance durability and write throughput.

**Tables**

`cache_tag_entries` — one row per (zone, tag, path) combination:

```sql
CREATE TABLE cache_tag_entries (
  zone  TEXT    NOT NULL,
  tag   TEXT    NOT NULL,
  path  TEXT    NOT NULL,
  mtime INTEGER NOT NULL,
  size  INTEGER NOT NULL,
  PRIMARY KEY (zone, tag, path)
);
CREATE INDEX cache_tag_entries_lookup ON cache_tag_entries (zone, tag);
```

`cache_tag_zones` — one row per zone, tracks bootstrap state:

```sql
CREATE TABLE cache_tag_zones (
  zone                TEXT    PRIMARY KEY,
  bootstrap_complete  INTEGER NOT NULL DEFAULT 0,
  last_bootstrap_at   INTEGER NOT NULL DEFAULT 0
);
```

**Read path.** A single-tag lookup is `SELECT DISTINCT path FROM cache_tag_entries WHERE zone = ? AND tag = ?`, backed by the `(zone, tag)` index. A multi-tag lookup uses an `IN (?, ?, …)` clause over the same index; the maximum number of bind parameters is 32766.

**Write path.** Each file update runs inside a `BEGIN IMMEDIATE` transaction: first a `DELETE` on `(zone, path)` removes stale entries, then one `INSERT OR REPLACE` per tag adds the new associations. Prepared statements are compiled once and reused.

**Storage estimate.** Each row is approximately 200 bytes on disk. A cache holding 10 000 files with an average of 10 tags per file produces roughly 20 MB of index data.

### Redis backend

The Redis backend implements the RESP protocol directly without an external client library. It communicates over a single TCP connection (`host:port`) or a Unix socket, with optional password authentication and database selection.

**Key structure**

| Key | Type | Contents |
|-----|------|----------|
| `cache_tag:zone:<zone>` | Hash | `bootstrap_complete` (0 or 1), `last_bootstrap_at` (Unix timestamp) |
| `cache_tag:tag:<zone>:<tag>` | Set | All cache file paths that carry this tag |
| `cache_tag:file:<zone>:<path>` | Set | All tags associated with this file |
| `cache_tag:filemeta:<zone>:<path>` | Hash | `mtime` and `size` for the cached file |

**Read path.** A single-tag purge queries `SMEMBERS cache_tag:tag:<zone>:<tag>`. A multi-tag purge uses `SUNION cache_tag:tag:<zone>:<tag1> cache_tag:tag:<zone>:<tag2> …` to collect all matching paths in one round trip.

**Write path.** When a file's tags are replaced the module first calls `SMEMBERS` on the file's tag set to get the old tags, issues `SREM` to remove the file path from each old tag set, then `SADD` to add it to each new tag set, rebuilds the file→tag set with `SADD`, and updates the metadata hash with `HMSET`. Deletes reverse the same steps. Commands within a batch are pipelined to reduce round-trip overhead. On a connection failure the module reconnects and retries up to two times before logging an error.

**Storage estimate.** Redis memory is proportional to the total number of (tag, path) associations stored across all sets, plus the overhead of the file-keyed sets and metadata hashes. There is no separate index structure; the sets are the index.

### Purge flow

When a tag `PURGE` request is received:

1. Tags are extracted from the request headers using the same tokenisation logic as indexing.
2. The index is queried for all file paths associated with the supplied tags (OR semantics — any matching tag is sufficient).
3. For each path the module applies the configured purge mode:
   - **Soft purge** — the cache file is marked expired in the shared-memory cache node so the next request is served as `EXPIRED`. The index entry is queued for async cleanup.
   - **Hard purge** — the cache file is deleted from disk immediately. The index delete is handed off to the owner worker via the shared-memory queue. A `200` response means the delete was accepted for processing, not necessarily already committed to the index backend.

## Configuration Examples

Use these as compact starting points after Quick Start.

### Same-location syntax

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }

    server {
        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
            proxy_cache_purge  $purge_method;
        }
    }
}
```

Use `soft` if you want matching entries to expire in place, or add `purge_all` if you want a purge request to target every cached entry in the zone.

### Separate-location syntax

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }

    server {
        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        tmpcache;
            proxy_cache_key    "$uri$is_args$args";
        }

        location ~ /purge(/.*) {
            proxy_cache        tmpcache;
            proxy_cache_key    "$1$is_args$args";
            proxy_cache_purge  $purge_method;
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
- separate-location purge with local `proxy_cache` and `proxy_cache_key` (`/separate` and `/purge_separate/...`)
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
