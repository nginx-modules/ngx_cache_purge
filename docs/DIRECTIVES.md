[ngx_cache_purge](../README.md)

# Directives

## `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Inline form** (`from …`) — intercepts the named HTTP method on a proxy location and purges the matching cache entry. `on` is a shorthand for method `PURGE`; `off` disables purging. Optionally restrict to a list of CIDR ranges or use `from all` to allow from any address. Adding `purge_all` before `from` empties the entire cache zone regardless of the request URI.

**Separate-location form** (zone and key arguments) — allowed only inside a purge location (typically a regex location capturing the cache key). Looks up the cache zone by name and purges the compiled key expression. This form is incompatible with `proxy_cache` and `proxy_pass` in the same location.

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for FastCGI cache zones configured with `fastcgi_cache` / `fastcgi_cache_path`.

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for SCGI cache zones configured with `scgi_cache` / `scgi_cache_path`.

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for uWSGI cache zones configured with `uwsgi_cache` / `uwsgi_cache_path`.

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Selects the successful PURGE body format. `Cache-Purge-Code` and the body result code agree; all module responses include `Cache-Control: no-store`. Errors use `application/problem+json` (RFC 9457). Binary or nonrepresentable keys use Base64 with an explicit encoding field; text keys are escaped.

`none` suppresses PURGE bodies: completed success becomes `204`, accepted work stays `202`, and errors retain 4xx/5xx. Status and REFRESH keep structured JSON. See the [HTTP contract](HTTP_RESPONSES.md) for the catalog, fields, examples and limits.

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Enables deferred wildcard and `purge_all` processing. A request returns `202 Accepted` after enqueueing or coalescing with an identical pending or running task. Exact-key purges are unaffected.

This switch controls legacy wildcard/all behavior. A `cache_purge_key` binding always uses deferred physical cleanup and logical `200` responses, even when this switch is off.

If the queue is full or a task allocation fails, the request stays open until admission succeeds. It performs no directory walk while waiting. A duplicate of an existing pending or running task can be accepted at capacity. Failure to allocate the request's small wait state returns `500`. Disconnect cleanup cancels the timer and releases the waiting gauge.

See [execution model](ARCHITECTURE.md#execution-model) for threads, reload and shutdown. For this flag and `cache_purge_vary_aware`, a repeated directive keeps the last value. `cache_purge_legacy_status` is obsolete and ignored. Invalid flag values are rejected.

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maximum number of outstanding tasks across all cache paths. Both pending and assigned tasks consume capacity. Duplicates reuse one allocation; a replay keeps that allocation and credit. Distinct tasks wait for a free credit.

The shared zone is created when background processing or a status endpoint is configured. Capacity changes require a restart, even when they round to the same zone size. Reload rejects resizing to retain accepted work. Adding cache roots can also require more zone memory and a restart. Drain pending and active work first when it must complete.

Declaring a namespace also creates the task queue.

Zone size includes task storage, path metadata and aligned worker metrics. It depends on the build and number of cache paths; see [memory sizing](OPERATIONS.md#memory-sizing).

## `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Selects the executor for accepted background tasks, with normal inheritance. For example:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX validates named pools and delivers completion through the native task event. No private thread-pool layout or completion queue is copied. Custom pool names must exist when threads are supported. Without thread support or on Windows the directive is accepted and execution uses bounded event-loop slices. A full selected pool delays execution; it does not move I/O back into the event loop. Dedicated pools add thread stacks and context-switch costs.

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Maximum tasks processed by each worker between throttle pauses. Progress is retained across traversal slices and thread completions. An empty queue ends the current batch. Values above 64 are clamped with a configuration warning; zero is rejected.

This controls tasks, not files or callback duration. Lower values introduce more pauses between tasks; they do not interrupt an active threaded walk. Use it together with `cache_purge_throttle_ms` when tuning disk load.

## `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Delay between background task batches. Accepts `10ms`, `500ms`, `1s` and quoted compound values such as `"2s 500ms"`. A bare integer means milliseconds: `10` and `10ms` are equivalent. Bare values produce a configuration warning; an explicit unit avoids it.

An idle worker checks again after ten times this interval. Incomplete slices and busy-mutex retries use 1 ms timers. Timer delays are minimum scheduling intervals, not deadlines. Zero and values exceeding the platform's safe timer range are rejected.

Admission, pool pressure and delayed cleanup retries use 10 ms polling.

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Serves a GET/HEAD JSON snapshot with `Cache-Control: no-store`. The path must identify a configured cache root; unknown paths fail configuration loading, even with background processing off. Restrict access to the endpoint:

```nginx
location = /cache_purge_queue_status {
    allow 127.0.0.1;
    deny  all;
    cache_purge_queue_status /var/cache/nginx;
}
```

```json
{
  "code": "status",
  "queue": { "size": 0, "running": 0, "max_size": 1024, "wait_time": 0, "full": false },
  "cache": {
    "pending": { "total": 0, "full": 0 },
    "active": 0,
    "requests": { "overflow": 0, "deduplicated": 0, "waiting": 0 },
    "files": { "deleted": 0, "errors": 0, "bytes": 0 },
    "logical": { "invalidations": 0 },
    "cleanup": { "scan_errors": 0, "retries": 0, "exhausted": 0 },
    "responses": { "hit": 0, "miss": 0, "hit_ratio": 0.000000 }
  }
}
```

`queue` describes the shared queue across all cache paths:

| Field | Type / unit | Meaning |
|---|---|---|
| `size` | Integer / tasks | Tasks waiting for a worker |
| `running` | Integer / tasks | Assigned tasks, including those waiting for a pool |
| `max_size` | Integer / tasks | Capacity for pending plus assigned tasks |
| `wait_time` | Integer / milliseconds | Oldest pending age; zero when empty |
| `full` | Boolean | Sampled pending plus assigned count has reached capacity |

`cache` describes the cache root selected by this location:

| Field | Type / unit | Meaning |
|---|---|---|
| `pending.total` | Integer / tasks | Pending tasks for this cache |
| `pending.full` | Integer / tasks | Pending `purge_all` tasks, included in `pending.total` |
| `active` | Integer / tasks | Assigned tasks until completion accounting finishes |
| `requests.overflow` | Integer / requests | Requests that entered admission waiting, including lock/allocation pressure |
| `requests.deduplicated` | Integer / requests | Requests coalesced with pending or running work |
| `requests.waiting` | Integer / requests | Current admission waiters for this cache |
| `files.deleted` | Integer / files | Successful background unlinks reported at pass completion |
| `files.errors` | Integer / errors | Background unlink failures other than an already missing file |
| `files.bytes` | Integer / bytes | Estimated logical sizes of successfully unlinked files |
| `responses.hit`, `responses.miss` | Integer / main requests | Core HIT and MISS decisions observed in log phase |
| `responses.hit_ratio` | Number / 0..1 | HIT / (HIT + MISS), or zero without either observation |
| `logical.invalidations` | Integer / decisions | Published generation invalidations, including duplicates and REFRESH decisions |
| `cleanup.scan_errors` | Integer / errors | Background metadata/open/read/close/allocation errors |
| `cleanup.retries` | Integer / passes | Additional passes scheduled because cleanup failed |
| `cleanup.exhausted` | Integer / tasks | Cleanup tasks retired after three failed retries |

`pending.full` counts full-cache operations. A wildcard whose key is `*` is a separate operation type. `active` includes pool waiters; active tasks are excluded from `queue.size` and `pending.total` but occupy admission capacity.

For a coalesced namespace sweep, `pending.full` reflects the first accepted task classification. REFRESH scans/probes are not included in the purge queue depth or deletion error counters. A rotated delayed retry can make `wait_time` a sampled head age rather than the maximum pending age.

Counters have one event-loop writer per process slot, with cache-line-aligned starts and strides. Threads return results through completion events. The status reader takes no queue mutex and performs no atomic fetch-add, including zero increments. On 64-bit builds, counters use plain aligned volatile loads and stores. On 32-bit builds, bounded sequence checks protect 64-bit reads; a busy shard may be omitted from one sample. Fields are approximate and are not a transaction across workers. Admission always uses locked control state, never monitoring values.

Cumulative counters saturate at the unsigned 64-bit maximum, survive ordinary reloads and reset on restart. The waiting gauge follows request cleanup. Synchronous operations and core cache-manager deletions are excluded from background deletion totals. Bytes are file-length estimates, not filesystem blocks or proof of disk space reclaimed: open readers and concurrent writes can change the result. Partial pass results are counted; crashed-worker results can be missing. These metrics are not an audit log.

HIT/MISS observation counts only main requests with a native cache decision. A missing upstream, cache status zero, BYPASS, EXPIRED, STALE, UPDATING and REVALIDATED are excluded from this ratio. Ordinary uncached requests do not become MISS. Monitoring alone can create the SHM zone even with background purging off. On 32-bit systems, queue age covers intervals below about 24.9 days; a negative signed clock difference is reported as zero.

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Accepted for configuration migration only. Both values emit a warning and have no effect. Remove the directive; invalid values still fail validation. A synchronous miss returns `200 no_match`; accepted physical cleanup returns `202 accepted`; published generation invalidation returns `200 invalidated`. The former `412`/`404` switch is gone. See the [HTTP contract](HTTP_RESPONSES.md).

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

When `on`, a successful exact-key purge walks the cache directory after deleting the primary file and removes any remaining files that carry the same `KEY:` string. This covers all `Vary` and `gzip_vary` variants of a cached response, which are stored at different filesystem paths but share one logical key. The byte immediately after the key string in each file is verified to be `\n`, preventing false matches against keys that share only a common prefix.

Disabled by default because each exact purge adds a full directory walk. Enable it for multiple `Vary`/`gzip_vary` representations. The walk also runs when the primary entry is already absent. Wildcard and `purge_all` already cover variants. Exact Vary keys are case-sensitive and limited to 511 bytes; longer keys return `400 invalid_selector` before any deletion.

Generation invalidation covers every representation of the logical key without this option.

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Groups logical keys that share invalidation rules. Declare the namespace before using it in `cache_purge_key`. Its name must contain 1–32 ASCII letters, digits, `_` or `-`. The shared-memory size must be at least eight system memory pages; this allocation is separate from the cleanup queue.

The state directory stores small marker files that distinguish full restarts. Keep it outside cache and temporary directories, and retain it for as long as the cache may exist. Creating a marker exclusively prevents reuse of an existing restart identifier. Deleting these markers while keeping cache files can make stale versions reachable again. No fsync/power-loss durability is promised.

An ordinary reload retains the namespace state. Changing its size or path, or removing the namespace, requires a full restart.

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Binds the location to an existing namespace and exposes `$cache_purge_key`. Set the entire `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` or `uwsgi_cache_key` to that variable, including the corresponding separate PURGE location. A PURGE whose expanded native key differs from the generated key fails with `500` instead of claiming strict invalidation. Do not refer to `$cache_purge_key` inside its own logical expression. Logical keys have a maximum of 8096 bytes. Bind only participating locations when other locations need legacy semantics; there is no `off` form to cancel an inherited binding.

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Creates an explicit main-request-only REFRESH endpoint; other methods return `405`. It requires a `cache_purge_key` binding, `$cache_purge_key` as its native expression, and a refresh profile. The expanded logical key selects an exact key, a trailing-`*` prefix or `*` for the namespace in this cache path. Protect the location with core access/authentication rules. Only proxy caches support REFRESH; PURGE remains available for all four protocols.

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Defines where HEAD probes go and how a logical cache key becomes an origin URI. The first argument is a literal internal proxy location. The second is the exact, case-sensitive prefix to remove from the logical key. The remainder must begin with `/`; query strings and escapes are retained. For example, prefix `catalog:` maps `catalog:/products/42?currency=EUR` to `/products/42?currency=EUR`.

The profile must check the same representation as the cached GET. Set `internal`, `proxy_cache off`, `proxy_intercept_errors off`, `proxy_pass_request_headers off`, `proxy_pass_request_body off` and `proxy_method HEAD`. Use a nonempty literal `Host` and literal representation headers. A client-controlled language, cookie or credential must not change what is being checked.

Set `If-None-Match` to exactly `$cache_purge_refresh_etag` and `If-Modified-Since` to exactly `$cache_purge_refresh_last_modified`. Use `$cache_purge_refresh_uri` in `proxy_pass` for the mapped URI. These variables provide probe data and are only meaningful in probe subrequests.

The module validates the actual target before access checks, including after internal redirects. It does not infer a host from a key or search for `://`. The administrator remains responsible for correct mapping. Vary entries are skipped because their original request headers are unavailable.

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Sets how long a page may keep starting new scan/probe work, including time spent waiting for its worker's scanner. On expiry, the page stops dispatching and reports incomplete work.

An active HEAD still finishes under `proxy_connect_timeout`, `proxy_send_timeout` and `proxy_read_timeout`; set those explicitly in the profile. This directive is not a hard response deadline and cannot interrupt a filesystem syscall.

Client disconnect and graceful shutdown stop further dispatch. If an I/O thread is still running, its storage remains allocated until NGINX reports completion.
