# ngx_cache_pilot

[![CI](https://github.com/wpelevator/ngx_cache_pilot/actions/workflows/ci.yml/badge.svg)](https://github.com/wpelevator/ngx_cache_pilot/actions/workflows/ci.yml) [![Bench](https://github.com/wpelevator/ngx_cache_pilot/actions/workflows/bench.yml/badge.svg)](https://github.com/wpelevator/ngx_cache_pilot/actions/workflows/bench.yml)

`ngx_cache_pilot` is an `nginx` module that adds cache purge support for [`fastcgi_cache`](https://nginx.org/en/docs/http/ngx_http_fastcgi_module.html), [`proxy_cache`](https://nginx.org/en/docs/http/ngx_http_proxy_module.html), [`scgi_cache`](https://nginx.org/en/docs/http/ngx_http_scgi_module.html), and [`uwsgi_cache`](https://nginx.org/en/docs/http/ngx_http_uwsgi_module.html) caches. A purge operation removes or expires cached content that matches the cache key, wildcard key, or configured cache tags for the request.

_This module is not distributed with the NGINX source. See [Installation Instructions](#installation-instructions)._

## Status

This is a fork of the [`ngx_cache_purge` module](https://github.com/nginx-modules/ngx_cache_purge) to add support for soft purgaging and cache tags (also known as surrogate keys).

Tagged upstream releases are the packaging input. Distribution packages should ship this module as a version-matched dynamic nginx module, not as a standalone binary intended to work across arbitrary nginx builds.

## Compatibility And Limits

- validated in source-build CI against nginx `1.26.3`, `1.28.3`, and `1.29.8`
- packaged installs must target the matching distro nginx ABI (for example Debian/Ubuntu `libnginx-mod-http-cache-pilot`)
- cache-tag indexing currently requires Linux
- packaged cache-tag indexing remains Linux-only even when the module itself is built as a dynamic module
- cache-tag and cache-key indexing use a single in-memory shared-memory backend configured with `cache_pilot_index_zone_size`
- index contents are rebuilt from cache files after a cold restart; they do not survive nginx process restarts
- index bootstrap is deferred while the nginx cache zone is cold (`cache loader` warmup); indexed purges become ready only after the zone is warm and the deferred bootstrap has been triggered and completed
- the current index lifecycle does not use an inotify watcher; index freshness is maintained by startup bootstrap plus nginx-native header-filter and log-phase hooks as cache files are written
- indexed tag purges require the cache zone to be registered with `cache_pilot_index on` and the shared-memory index for that zone to be ready; if deferred bootstrap has not yet been finalized after loader warmup, purge/key-index request paths may trigger that bootstrap check/finalization work
- `--with-threads` is strongly recommended so wildcard purge scans do not block the nginx event loop
- nginx must be built with HTTP cache support enabled; configuring nginx with `--without-http-cache` is unsupported for this module
- dynamic-module deployments still depend on the corresponding nginx cache modules such as `ngx_http_proxy_module`, `ngx_http_fastcgi_module`, `ngx_http_scgi_module`, and `ngx_http_uwsgi_module`; if those are loadable on your platform, load them before `ngx_http_cache_pilot_module`

## Installation Instructions

Use a distro-native package when one is available. Otherwise build against the exact nginx source or nginx development package that matches the target system.

This module requires nginx HTTP cache support. A configure run that uses `--without-http-cache` will fail immediately when this module is added.

For most users, the recommended installation path is a dynamic module package built for the exact nginx version or nginx ABI already installed on the target system.

### Packaged install (Debian/Ubuntu)

The Debian/Ubuntu packaging model is a distro-native dynamic module package named `libnginx-mod-http-cache-pilot`.

- build the package against the target distro nginx ABI
- install it alongside the matching packaged nginx build
- let the package manager place `ngx_http_cache_pilot_module.so` under `/usr/lib/nginx/modules/`
- let the package manager enable `mod-http-cache-pilot.conf` under `/etc/nginx/modules-enabled/`

When a repository or PPA publishes this package, the install flow is:

```bash
sudo apt install nginx libnginx-mod-http-cache-pilot
sudo nginx -t
sudo systemctl reload nginx
```

If your distro splits required upstream nginx cache modules into separate dynamic-module packages, make sure those modules are enabled before `mod-http-cache-pilot.conf`.

### Recommended: build a dynamic module for your installed NGINX version

- Check the target version with `nginx -v`.
- Download the matching NGINX source release.
- Build this repository as a dynamic module against that exact source tree.
- Copy the resulting `.so` into your NGINX modules directory and load it with `load_module`.

For example, if `nginx -v` reports `nginx/1.28.1`:

```bash
cd ~/build/nginx-cache-pilot
wget https://nginx.org/download/nginx-1.28.1.tar.gz
tar xf nginx-1.28.1.tar.gz
cd nginx-1.28.1

./configure \
    --with-compat \
    --with-threads \
    --add-dynamic-module=../ngx_cache_pilot

make modules
```

This produces `objs/ngx_http_cache_pilot_module.so`, which you can then copy into your nginx modules directory and load with `load_module`.

Dynamic-module builds still depend on the corresponding nginx upstream cache modules such as `ngx_http_proxy_module`, `ngx_http_fastcgi_module`, `ngx_http_scgi_module`, and `ngx_http_uwsgi_module`. In packaged deployments those modules must either be built into nginx or loaded before `ngx_http_cache_pilot_module.so`.

### Enabling and loading the module

Source builds typically add this line near the top of `nginx.conf`:

```nginx
load_module modules/ngx_http_cache_pilot_module.so;
```

Packaged Debian/Ubuntu installs should use the package-managed module load file instead of a hand-edited `load_module` line:

- module binary: `/usr/lib/nginx/modules/ngx_http_cache_pilot_module.so`
- module load file: `/usr/share/nginx/modules-available/mod-http-cache-pilot.conf`
- enabled symlink: `/etc/nginx/modules-enabled/50-mod-http-cache-pilot.conf`

If a dynamic-module deployment fails at runtime with a missing `ngx_modules` symbol, that usually indicates a packaging or build-flow problem outside the normal nginx `auto/module` path rather than an index-store configuration issue in this repository.

### Alternative: build NGINX from source with this module

If you are building your own NGINX binary from source, point `./configure` at this repository with `--add-module` for a static build or `--add-dynamic-module` for a dynamic build.

```bash
./configure \
    --with-debug \
    --with-threads \
    --with-http_ssl_module \
    --add-module=/path/to/ngx_cache_pilot
make
make install
```

For a dynamic module build in this workflow, replace `--add-module` with `--add-dynamic-module` and use `make modules`.

The repository `config` script does not require an external index-store dependency. The tag index lives in an nginx shared-memory zone managed by the module itself.

`--with-threads` enables nginx's thread pool support. When present, the module offloads wildcard/partial-key purge scans to a worker thread. Without `--with-threads` these operations run synchronously in the event loop.

If you want the included containerized build environment, tests, or the manual validation setup, see [Development](#development).

## Quick Start

`ngx_cache_pilot` supports multiple purge styles depending on how you want to address cached content:

- exact URI purge
- wildcard URI purge using a trailing `*`
- cache-tag and surrogate-key purge

When `cache_pilot_index_zone_size` and `cache_pilot_index` are enabled for a zone, the module also maintains a cache-key index. That key index is used to:

- fan out exact-key hard purges across files that share the same key (for example, `Vary` variants)
- serve wildcard key purges from in-memory key metadata before falling back to filesystem walking

For most users, the simplest starting point is a cached location plus a `PURGE` method restricted to trusted clients.

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;

    # Restrict purging to specific IPs and the PURGE method.
    map $request_method:$remote_addr $purge_request {
        default         off;
        PURGE:127.0.0.1 on;
    }

    server {
        listen 8080;

        location / {
            proxy_pass http://127.0.0.1:8000;
            proxy_cache tmpcache;
            proxy_cache_purge $purge_request;
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

If you want cache-tag purging, allocate an index zone and watch the cache directory:

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;

    # Storage for cache tag and cache-key index metadata.
    cache_pilot_index_zone_size  32m;

    map $request_method:$remote_addr $purge_request {
        default         off;
        PURGE:127.0.0.1 on;
    }

    server {
        location /tagged/ {
            proxy_pass http://127.0.0.1:8000;
            proxy_cache tmpcache;

            # Do hard purge by default.
            proxy_cache_purge $purge_request;

            # Use this header to enable soft purge.
            cache_pilot_purge_mode_header X-Purge-Mode;

            # Enable cache tag indexing and purging.
            cache_pilot_index on;
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

## Configuration Examples

Use these as compact starting points after Quick Start.

### Same-location syntax

```nginx
http {
    proxy_cache_path /tmp/cache keys_zone=tmpcache:10m;
    map $request_method:$remote_addr $purge_request {
        default         off;
        PURGE:127.0.0.1 on;
    }

    server {
        location / {
            proxy_pass http://127.0.0.1:8000;
            proxy_cache tmpcache;
            proxy_cache_purge $purge_request;
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
            proxy_pass http://127.0.0.1:8000;
            proxy_cache tmpcache;
        }

        location ~ /purge(/.*) {
            allow 127.0.0.1;
            deny all;

            proxy_cache tmpcache;
            proxy_cache_purge $purge_request;
        }
    }
}
```

### Response types

Use `cache_pilot_purge_response_type` to switch between `json` and `text` responses in the scope where the purge response is generated.

### Cache tags

The minimal cache-tag setup is already shown in Quick Start. Use that pattern whenever you want to purge by `Cache-Tag` or `Surrogate-Key` headers.

## Configuration Reference

Directive names documented in this section are part of the module's public configuration API and are intended to stay stable. New configuration knobs should extend one of the existing directive families rather than introduce parallel synonyms or replacement spellings:

- `fastcgi_cache_purge`, `proxy_cache_purge`, `scgi_cache_purge`, and `uwsgi_cache_purge` for upstream-cache purge integration
- `cache_pilot_*` for module-owned features such as indexing, tag handling, purge response behavior, metrics, and tuning

### Directives

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

For same-location syntax, the trailing purge method token is stored as a literal string at config time; it is not compiled as a complex value. In practice that means values such as `$foo` are not evaluated there.

### Optional directives

#### `cache_pilot_purge_response_type`

- **syntax**: `cache_pilot_purge_response_type json|text`
- **default**: `json`
- **context**: `http`, `server`, `location`

Set the response type returned after a purge.

When `json` is selected, successful purges may also include `cache_pilot.purge_path` to describe the request path that completed the purge, for example `filesystem-fallback`, `key-prefix-index`, `reused-persisted-index`, or `exact-key-fanout`. Here, `reused-persisted-index` means the request reused shared-memory index state that was already built for the current nginx lifetime; it does not imply on-disk persistence. JSON responses also include `cache_pilot.purged`, using the same `exact`, `wildcard`, `tag`, and `all` buckets with `hard` and `soft` counts to report how many cache entries that purge request removed or expired. Text responses keep existing plain body format.

#### `cache_pilot_purge_mode_header`

- **syntax**: `cache_pilot_purge_mode_header <header>`
- **default**: `none`
- **context**: `http`, `server`, `location`

Enable request-time soft/hard purge override using the named request header. When unset, purge mode is controlled only by the configured `soft` flag.

If configured:

- header value `soft`, `true`, or `1` forces a soft purge
- any other present value forces a hard purge
- if the header is absent, the configured purge mode is used
- `purge_all` ignores this override and keeps its configured behavior

This header only switches soft versus hard mode after the request has already matched a configured purge path. It does not enable purge conditionally on its own.

#### `cache_pilot_index_zone_size`

- **syntax**: `cache_pilot_index_zone_size <size>`
- **default**: `none`
- **context**: `http`

Allocate the shared-memory zone used for cache-tag indexing and cache-key metadata. This feature is currently Linux-only.

The index is process-local shared state managed by nginx workers. It is rebuilt from cache files after a cold restart and does not survive nginx process restarts.

In practice, the zone survives worker reuse within the same running nginx instance, but it is rebuilt after a full process restart.

This zone also stores cache-key metadata used by key-based purge acceleration:

- exact-key hard purge fanout across sibling files sharing one cache key
- wildcard key-prefix candidate matching

The same zone also stores the shared-memory tag index used by cache-tag purges. Shared-memory usage therefore scales with:

- the number of cached files tracked by the index
- the number of distinct exact cache keys tracked for fanout
- the number of distinct tags
- the total number of file-tag relations

#### `cache_pilot_tag_headers`

- **syntax**: `cache_pilot_tag_headers <header> [header ...]`
- **default**: `Surrogate-Key Cache-Tag`
- **context**: `http`, `server`, `location`

Set the request and cached-response headers used for cache-tag extraction and tag purge matching.

All watched locations that share the same cache zone must use the same `cache_pilot_tag_headers` list.

#### `cache_pilot_index`

- **syntax**: `cache_pilot_index on|off`
- **default**: `on` when `cache_pilot_index_zone_size` is configured and the location uses upstream cache, otherwise `off`
- **context**: `http`, `server`, `location`

Enable cache-tag indexing for the cache used by the current purge-enabled location. When enabled, the module bootstraps index metadata from cache files at startup and allows tag-based `PURGE` requests once that bootstrap is ready. After startup, newly cacheable upstream responses are indexed by nginx-native header-filter and log-phase hooks, so updates are visible almost immediately without filesystem watch polling.

At startup, index bootstrap waits until the nginx cache zone is no longer cold, then performs a one-time cache-tree scan before indexed tag purges for that zone are considered ready.

When `cache_pilot_index_zone_size` is also configured, cached response writes also update the shared-memory exact-key index used by exact-key fanout. Wildcard key-prefix purge paths reuse the same in-memory file metadata, but do not yet use a dedicated prefix tree.

Set `cache_pilot_index off;` to opt out on locations where indexing should stay disabled.

For hard tag purges, matching cache files are removed immediately and their in-memory index entries are deleted in the same purge path.

#### `cache_pilot_stats`

- **syntax**: `cache_pilot_stats [zone ...]`
- **default**: `none`
- **context**: `location`

Expose a read-only metrics endpoint for the configured cache zones. With no arguments, all zones known to the module are included. One or more zone names can be listed to restrict the output.

The endpoint returns `Cache-Control: no-store` and supports two output formats:

| Trigger                                                                               | Format          | Content-Type                |
| ------------------------------------------------------------------------------------- | --------------- | --------------------------- |
| Default, `?format=json`, or `Accept: application/json`                                | JSON            | `application/json`          |
| `?format=prometheus`, `Accept: text/plain`, or `Accept: application/openmetrics-text` | Prometheus text | `text/plain; version=0.0.4` |

Example configuration:

```nginx
location /_cache_stats {
    cache_pilot_stats;
}
```

Or filtered to specific zones:

```nginx
location /_cache_stats {
    cache_pilot_stats my_cache other_cache;
}
```

**JSON response structure** (captured from live `/_stats?format=json` output; `zones` list truncated):

```json
{
  "version": 1,
  "timestamp": 1776605478,
  "purges": {
    "exact": {
      "hard": 0,
      "soft": 0
    },
    "wildcard": {
      "hard": 0,
      "soft": 0
    },
    "tag": {
      "hard": 0,
      "soft": 6
    },
    "all": {
      "hard": 0,
      "soft": 0
    }
  },
  "purged": {
    "exact": {
      "hard": 0,
      "soft": 0
    },
    "wildcard": {
      "hard": 0,
      "soft": 0
    },
    "tag": {
      "hard": 0,
      "soft": 6
    },
    "all": {
      "hard": 0,
      "soft": 0
    }
  },
  "key_index": {
    "exact_fanout": 0,
    "wildcard_hits": 0
  },
  "zones": {
    "zone-one": {
      "size": 35184,
      "max_size": 2251799813685247,
      "cold": false,
      "entries": {
        "total": 4815,
        "valid": 0,
        "expired": 4815,
        "updating": 0
      },
      "index": {
        "state": "ready",
        "state_code": 2,
        "last_updated_at": 1776605478,
        "backend": "shm"
      }
    }
  }
}
```

Additional zones are omitted for brevity.

`zones.<zone>.max_size` reports the configured NGINX cache zone limit. When the in-memory index is enabled, `index.max_size` reports the configured `cache_pilot_index_zone_size` shared-memory limit for the index and `index.last_updated_at` reports the Unix epoch timestamp of the last index mutation observed for that zone. `index` is omitted when the in-memory index is unavailable. `index.state_code` uses `0=disabled`, `1=configured`, and `2=ready`. `index.backend` is currently always `"shm"`. `purges` counters are global across all zones and survive `nginx -s reload`. `purged` uses the same `exact`, `wildcard`, `tag`, and `all` buckets with `hard` and `soft` counts for cumulative cache entries removed or expired by each purge path.

**Prometheus metrics** (prefix `nginx_cache_pilot_`):

- `nginx_cache_pilot_purges_total{type,mode}` — counter, purge operations by type (`exact`, `wildcard`, `tag`, `all`) and mode (`hard`, `soft`)
- `nginx_cache_pilot_purged_entries_total{type,mode}` — counter, cache entries removed or expired by purge type (`exact`, `wildcard`, `tag`, `all`) and mode (`hard`, `soft`)
- `nginx_cache_pilot_key_index_total{type}` — counter, key-index assisted purge operations by type (`exact_fanout`, `wildcard_hits`)
- `nginx_cache_pilot_zone_size_bytes{zone}` — gauge, current zone usage in bytes
- `nginx_cache_pilot_zone_max_size_bytes{zone}` — gauge, configured maximum NGINX cache zone size
- `nginx_cache_pilot_zone_cold{zone}` — gauge, 1 while the cache loader is still warming the zone
- `nginx_cache_pilot_zone_entries{zone,state}` — gauge, entry count by state (`valid`, `expired`, `updating`)
- `nginx_cache_pilot_index_max_size_bytes{zone}` — gauge, configured maximum shared-memory cache index size
- `nginx_cache_pilot_index_last_updated_at_seconds{zone}` — gauge, Unix epoch timestamp of the last in-memory index update for the zone
- `nginx_cache_pilot_index_state{zone,state}` — gauge, per-zone key index readiness (`0=disabled`, `1=configured`, `2=ready`)
- `nginx_cache_pilot_index_info{zone,backend}` — info gauge, tag index backend type

## Partial Keys

Sometimes it is not possible to pass the exact cache key to purge a page. For example, parts of the key may depend on cookies or query parameters. You can specify a partial key by adding an asterisk at the end of the URL.

```bash
curl -X PURGE /page*
```

The asterisk must be the last character of the key, so you must put the `$uri` variable at the end of the configured cache key.

When key-index metadata is available and ready for the zone, wildcard purges use in-memory key metadata first. That avoids a filesystem walk, but it is still a linear scan over indexed file metadata for the zone rather than a dedicated prefix index. If key-index data is unavailable for the zone, wildcard purges fall back to the existing full cache tree walk.

## Exact-Key Purge Fanout

With `cache_pilot_index_zone_size` and `cache_pilot_index` enabled for a zone, exact-key purge can fan out to all files that share the same cache key, including `Vary` variants.

Behavior summary:

- exact-key hard purge always removes the directly resolved cache file
- exact-key soft purge always expires the directly resolved cache file
- when key-index data is ready for the zone, exact-key hard and soft purge also fan out to sibling files sharing the same key
- if key-index data is unavailable or not yet ready, exact-key purge does not do a full cache scan

## Soft Purge

By default, soft purge behavior is still controlled by the configured `soft` parameter.

If `cache_pilot_purge_mode_header` is configured, exact-key, wildcard, and cache-tag / surrogate-key purges can override that mode per request. A value of `soft`, `true`, or `1` forces a soft purge; any other present value forces a hard purge.

The `soft` config parameter still controls `purge_all`, which does not honor `cache_pilot_purge_mode_header`.

- Exact-key soft purge marks the cached entry as expired, so the next request is handled as `EXPIRED` rather than a deletion-driven `MISS`; when key-index fanout is available it applies this expiration to sibling variants sharing the same key.
- Wildcard soft purge applies the same expiration behavior to all matching keys.
- `purge_all` can also be combined with `soft` to expire every cached entry in a zone without removing the underlying cache files immediately.

For wildcard and `purge_all` soft purges, the module expires both the cache-file header on disk and the matching shared-memory cache node so the next lookup is treated as expired consistently.

For high-traffic keys, soft purge should be deployed with nginx cache locking to prevent thundering-herd refresh bursts:

- `proxy_cache_lock on;`
- `proxy_cache_use_stale updating;`

Without those settings, concurrent requests that observe an expired object can still fan out to upstream simultaneously.

## Cache Tags

The module can also purge cached objects by cache tag, similar to `Surrogate-Key` or `Cache-Tag` support in other reverse proxies.

When `cache_pilot_index_zone_size` and `cache_pilot_index` are enabled:

- cached response files are parsed for the headers listed in `cache_pilot_tag_headers`
- `Surrogate-Key` values are parsed as comma- or whitespace-delimited tags
- `Cache-Tag` values are parsed as comma- or whitespace-delimited tags
- the module stores a shared-memory tag-to-cache-file index
- the module stores a shared-memory exact-key index used for exact-key fanout across sibling files sharing one cache key
- the module stores per-file cache-key metadata reused by wildcard key-prefix purge
- startup bootstrap is deferred until the nginx cache loader has warmed the target zone (`cold` becomes false)

To purge by tag, send a normal `PURGE` request and include one or more tag headers:

```bash
curl -i -X PURGE -H 'Surrogate-Key: article-42 group-a' 'http://127.0.0.1/tagged/item'
curl -i -X PURGE -H 'Cache-Tag: article-42, group-a' 'http://127.0.0.1/tagged/item'
curl -i -X PURGE -H 'Surrogate-Key: article-42 group-a' -H 'X-Purge-Mode: soft' 'http://127.0.0.1/tagged/item'
curl -i -X PURGE -H 'Cache-Tag: article-42, group-a' -H 'X-Purge-Mode: soft' 'http://127.0.0.1/tagged/item'
```

All supplied tags are matched with OR semantics. If any cached file is indexed under any supplied tag, it will be purged.

If a watched purge location receives a plain `PURGE` request without any of the configured tag headers, the module falls back to the normal key-based purge behavior for that location.

For tag-based purges, the configured `cache_pilot_purge_mode_header` can switch a request between soft and hard purge. Without that header, the configured purge mode is used.

Hard tag purges use asynchronous owner-worker handoff for backend index deletes. A `200` response means the delete work was accepted for processing, not necessarily already applied to the in-memory index yet.

Transient index-maintenance failures during a tag purge are logged and do not by themselves turn an otherwise successful purge into a `500`; the request result reflects whether matching cache files were purged, not whether every in-memory update succeeded.

Notes:

- Cache-tag support currently requires Linux.
- The tag index lives in a shared-memory zone sized by `cache_pilot_index_zone_size`.
- After a cold restart the index is rebuilt from cache files during startup. Indexed tag purges decline until that rebuild has completed for the target zone.
- Rebuild time is roughly linear in the number of cache files and the cost of reading their headers, so large caches can take seconds to minutes to become ready after restart. Use `cache_pilot_stats` to gate deploy or purge automation on readiness.
- Current startup logic defers index bootstrap until the cache zone is warm (`cold=0`) and then performs a one-time bootstrap scan.
- After startup, response writes update the index through nginx-native hooks (header filter + log phase) in the worker handling the request.
- When built with `--with-threads`, wildcard purge scans run in an nginx thread pool, keeping the event loop unblocked. Without threads, wildcard scans run synchronously.

## Known issues

- Exact-key fanout across `Vary` variants depends on key-index readiness for the zone. If key-index data is unavailable or not yet ready, exact-key purge targets only the directly resolved cache file and does not run a full cache scan.
- Wildcard purges still use a linear scan over in-memory key metadata when the index is ready. They avoid a filesystem walk in that case, but they are not yet backed by a dedicated prefix index.
- Soft purge does not enforce `proxy_cache_lock` or `proxy_cache_use_stale updating`; if those nginx directives are not enabled, expiring a hot object can still trigger concurrent upstream refreshes.
- Cache manager expirations can leave stale index entries temporarily; they are removed lazily when a purge path encounters ENOENT for the file.

## Cache Index Architecture

This section describes how the cache index works internally. It is not required reading for normal use, but is useful when diagnosing storage growth, planning capacity, or modifying the module.

### Overview

The cache index stores both:

- tag-to-path associations for cache-tag and surrogate-key purge
- exact-key associations used for fanout across sibling files sharing one cache key
- per-file cache-key metadata reused by wildcard key-prefix matching

Without index data, tag purge would have no efficient path lookup, and wildcard key purge relies on filesystem walking.

### Index population

The index is built through startup bootstrap and near-real-time hook updates.

**Loader-aligned startup bootstrap.** After a restart, bootstrap waits until nginx marks the cache zone warm (`cold=0`). The module then scans the cache directory tree, reads cached response headers, extracts tags, and rebuilds the shared-memory index before indexed tag purges for that zone are considered ready. Requests do not trigger a synchronous request-path bootstrap anymore.

**Near-real-time refresh.** A header filter captures tag headers for cacheable upstream responses, and a log-phase handler commits file metadata directly into shared memory when the response completes.

Operationally, the startup sequence is now: wait for loader warmup, let the one-time bootstrap scan finish, and then maintain freshness via hook-driven updates on cache writes. For hot objects, pair soft purge with `proxy_cache_lock on;` and `proxy_cache_use_stale updating;` to keep a single upstream refresh in flight.

**Tag extraction.** Both paths use the same extraction logic. The module reads the configured header names (`Surrogate-Key` and `Cache-Tag` by default) from the binary nginx cache file header and splits the value on commas and ASCII whitespace. Duplicate tags within a single response are deduplicated. The hard limit is 1000 extracted tag tokens per scan.

### Shared-memory index

The module stores tag associations, exact-key associations, per-file cache-key metadata, and per-zone bootstrap state inside an nginx shared-memory zone created by `cache_pilot_index_zone_size`.

This shared-memory store is currently the only supported index backend. The module does not expose backend selection or pluggable storage configuration.

**Read path.** Tag purges look up matching cache paths from the in-memory tag index, but only after the zone has reached the ready state. Exact-key fanout looks up sibling paths from the in-memory exact-key index. Wildcard key-prefix purges read in-memory per-file key metadata before deciding whether a filesystem walk is needed.

**Write path.** Hard purges delete the cache file and remove the corresponding in-memory entry in the same request path. Startup bootstrap repopulates index state for existing files after loader warmup.

**Storage estimate.** Shared-memory usage scales with the number of cached files, the number of distinct exact keys and tags, the total number of file-tag relations, and the size of stored cache keys and paths. Exact-key fanout now pays extra metadata per tracked file, and tag purge now pays extra metadata per file-tag relation. Start with `32m` for moderate tag usage and increase it if your cache holds a large number of tagged objects or many tags per object.

### Purge flow

When a tag `PURGE` request is received:

1. Tags are extracted from the request headers using the same tokenisation logic as indexing.
2. If the zone is not registered for indexing or its shared-memory index is not yet ready, the purge declines instead of running a synchronous cache-tree bootstrap inside the request.
3. Otherwise, the index is queried for all file paths associated with the supplied tags (OR semantics — any matching tag is sufficient).
4. For each path the module applies the configured purge mode:
   - **Soft purge** — the cache file is marked expired in the shared-memory cache node so the next request is served as `EXPIRED`.
   - **Hard purge** — the cache file is deleted from disk immediately and the corresponding in-memory index entry is removed in the same purge path.

## Development

Use this section if you are hacking on the module, running the automated test suite, or validating behavior inside the included container.

The repository includes a containerized build environment with:

- Debian-based build tooling for NGINX modules
- a separate Debian packaging container with `nginx-dev`, `nginx-core`, and Debian package build tools
- downloaded NGINX source in `/opt/nginx-src/nginx-$NGINX_VERSION`
- prebuilt default NGINX binary at `/opt/nginx/sbin/nginx`
- `Test::Nginx` installed from `openresty/test-nginx`

### Development container

Use the included container for development, testing, and manual validation. It is not the primary installation path for matching a system-provided NGINX package.

```bash
make shell
make nginx-build
make nginx-version
```

Use the packaging container when you want to build and smoke-test the Debian package locally without installing packaging dependencies on the host:

```bash
docker compose build packaging
docker compose run --rm packaging make debian-package-smoke
```

### GitHub Codespaces

The repository also includes a `.devcontainer/devcontainer.json` that builds a GitHub Codespaces devcontainer from the repository `Dockerfile`.

After the Codespace is created, open a terminal in the repository root and run the usual targets directly. The docker-compose workflow described elsewhere in this README is for local development only; Codespaces uses the devcontainer directly, so docker-compose is not applicable there.

```bash
make nginx-build
make test
make bench-quick
```

### Common development commands

```bash
make format
make test
make bench-quick
```

### Versioning and releases

Treat the Git tag as the upstream module version. Use plain semantic versions such as `1.2.0`; the tag, GitHub release, and `CHANGELOG.md` release entry should all match.

Use `debian/changelog` for the Debian package version. For an upstream `1.2.0` release, the first package upload should be `1.2.0-1`. If you need to rebuild or republish the same upstream release without changing the upstream version, bump only the Debian revision (`1.2.0-2`, `1.2.0-3`, and so on). Keep the release notes aligned across `CHANGELOG.md` and `debian/changelog`, but do not collapse them into one file: Debian tooling reads `debian/changelog` directly, so it needs to remain in Debian's package changelog format even when it is summarizing the same release.

Before tagging a release, run the usual validation flow from this repository:

```bash
docker compose build
docker compose run --rm dev make format
docker compose run --rm dev make test
docker compose run --rm packaging make debian-package-smoke
```

Release checklist:

1. Move release notes out of `CHANGELOG.md`'s `## UNRELEASED` section into a dated release entry and reopen `## UNRELEASED` for follow-up work.
2. Update the top entry in `debian/changelog` to the release version, keeping it in sync with the same release notes from `CHANGELOG.md` in Debian changelog format.
3. Commit the release preparation.
4. Create an annotated Git tag for the upstream version, for example `git tag -a 1.2.0 -m "Release 1.2.0"`.
5. Push the release commit and tag, then publish the GitHub release from that tag.

### Benchmark suite

The repository includes a container-only benchmark harness under `bench/` for measuring purge performance under concurrent GET load. Feature validation for key-index readiness, exact-key fanout, and wildcard key-prefix assist now lives in the regular `Test::Nginx` suite (`t/proxy_key_index.t`), so the benchmark stays focused on steady-state throughput and latency.

By default it runs all benchmark scenarios in a single run (and one summary table):

- exact-key soft purge baseline (`exact-baseline`)
- cache-tag soft purge with shm index
- exact soft purge with index disabled (`exact-scan`)
- exact soft purge with index enabled and `Vary` siblings (`exact-index`)
- wildcard soft purge with index disabled (filesystem walk, `wildcard-scan`)
- wildcard soft purge with index enabled (key-prefix assist when ready, `wildcard-index`)

Each scenario warms 1000 cached objects, starts 50 keep-alive GET workers, then runs a sequential PURGE worker in parallel while collecting:

- GET throughput and latency percentiles
- cache hit rate and `X-Cache-Status` breakdown
- purge throughput and latency percentiles
- `cache_pilot_stats` snapshots before and after the run

Run the quick suite after building nginx:

```bash
make shell
make nginx-build
make bench-quick
make bench
cat ./bench/results/latest/summary.txt
```

Results are written under `bench/results/<timestamp>/` with one JSON file per scenario plus `summary.json`, `summary.txt`, and nginx log artifacts. The `bench/results/latest` symlink points at the most recent run. The runner always creates an aggregated `nginx_error.log` plus per-startup and per-scenario `*_nginx_error.log` files so CI artifact paths stay stable; when nginx emits log output, `bench/bench.pl` also prints that chunk inline and appends it to those files.

The benchmark suite uses a single nginx runtime per run. It renders `bench/nginx.conf`, starts nginx once, and executes all selected scenarios against that runtime.

`bench/bench.pl` can also fail the run on threshold regressions with `--assert-file <path>`. The default assertion file is JSON with optional `defaults` and per-scenario rules under `scenarios`, keyed by scenario ids (for example `exact-baseline`, `tag-shm`, `exact-scan`, `exact-index`, `wildcard-scan`, and `wildcard-index`). Metrics use dot-paths into the summary object, for example `get.rps`, `get.cache_hit_rate`, `get.latency_us.p95`, and `purge.rps`. Each rule supports `min` and/or `max`. See `bench/assertions.example.json` for the current performance thresholds.

### Docker Validation Config

For manual validation inside the development container, the repository includes an example nginx configuration at `examples/kitchen-sink.conf`.

It uses the shared-memory index directive for tag indexing:

```nginx
cache_pilot_index_zone_size  32m;
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
- cache metrics via `cache_pilot_stats` (`/_stats`)

Start it inside the container after building nginx:

```bash
make shell
make nginx-build
rm -rf /tmp/ngx_cache_*
/opt/nginx/sbin/nginx -p /tmp -c ./examples/kitchen-sink.conf
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

Watched-location plain `PURGE` fallback:

```bash
curl -i 'http://127.0.0.1:8080/tagged/plain'
curl -i -X PURGE 'http://127.0.0.1:8080/tagged/plain'
curl -i 'http://127.0.0.1:8080/tagged/plain'
```

The final request should return `X-Cache-Status: EXPIRED`, showing that a watched location still falls back to key-based soft purge when no tag headers are supplied.

Custom `cache_pilot_tag_headers` flow:

```bash
curl -i 'http://127.0.0.1:8080/tagged_custom'
curl -i -X PURGE -H 'Custom-Group: custom-alpha' 'http://127.0.0.1:8080/tagged_custom'
curl -i 'http://127.0.0.1:8080/tagged_custom'
```

The final request should return `X-Cache-Status: EXPIRED`, confirming that both cached-response indexing and purge matching use `Edge-Tag` and `Custom-Group` for that isolated zone.

Cache metrics endpoint:

```bash
curl -s 'http://127.0.0.1:8080/_stats'
curl -s 'http://127.0.0.1:8080/_stats?format=prometheus'
curl -s -H 'Accept: text/plain' 'http://127.0.0.1:8080/_stats'
```

After running some purge requests, re-fetch the endpoint and verify the `purges` counters increment.

Stop the validation nginx instance with:

```bash
kill "$(cat /tmp/ngx-cache-pilot-validation.pid)"
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
