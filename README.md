# ngx_cache_purge

[English](README.md) · [简体中文](docs/i18n/README.zh-CN.md) · [日本語](docs/i18n/README.ja.md) · [한국어](docs/i18n/README.ko.md) · [Português (Brasil)](docs/i18n/README.pt-BR.md) · [Deutsch](docs/i18n/README.de.md) · [Español](docs/i18n/README.es.md) · [Français](docs/i18n/README.fr.md) · [Türkçe](docs/i18n/README.tr.md) · [Polski](docs/i18n/README.pl.md) · [Bahasa Indonesia](docs/i18n/README.id.md) · [Tiếng Việt](docs/i18n/README.vi.md) · [Українська](docs/i18n/README.uk.md) · [فارسی](docs/i18n/README.fa.md) · [Русский](docs/i18n/README.ru.md)

An NGINX module for `FastCGI`, `proxy`, `SCGI` and `uWSGI` cache purging. Version 4.0 retains the purge configuration syntax, standardizes HTTP results and adds opt-in generation invalidation before deferred unlink, plus bounded conditional REFRESH for proxy caches.

---

## Contents

- [Features](#features)
- [Compatibility](#compatibility)
- [Installation](#installation)
- [Execution model](#execution-model)
- [Directives](#directives)
- [Partial key purge](#partial-key-purge)
- [Generation invalidation](#generation-invalidation)
- [Conditional REFRESH](#conditional-refresh)
- [Sample configurations](#sample-configurations)
- [Performance tuning](#performance-tuning)
- [Monitoring and debugging](#monitoring-and-debugging)
- [Troubleshooting](#troubleshooting)
- [Testing](#testing)
- [Migration](#migration)
- [Temporary directories](#temporary-directories)
- [Security](#security)
- [License](#license)

---

## Features

- **Inline purge** — dedicated HTTP method (`PURGE`) with IP access control
- **Separate purge location** — zone/key `proxy_cache_purge zone key` syntax for
  regex-captured key purging without a `proxy_pass`
- **Wildcard / partial purge** — trailing `*` walks the cache directory and
  removes all matching entries
- **`purge_all`** — removes every entry in the cache zone in one request
- **Background queue** — deferred traversal with bounded event-loop slices
  or an optional NGINX thread pool; configurable task batches and pauses
- **Vary-aware purge** — after an exact-key purge, removes all filesystem
  variants (gzip, Vary header) sharing the same cache key
- **Response types** — `json` (default), `html`, `xml`, `text`, `none`; stable result codes in headers and bodies
- **Named execution pools** — inherited pool selection with incremental fallback
  on builds without threads and on Windows
- **Active-task deduplication** — one indexed operation and at most one pending
  replay for repeated purges received during a walk
- **Status** — per-cache queue, deletion and HIT/MISS metrics, read without
  taking the shared queue mutex
- **Generation keys** — exact, prefix and namespace invalidation without request-bound disk I/O; late old writes remain unreachable
- **Conditional REFRESH** — explicit HEAD profile, shared probe budget and 32-entry continuation pages

---

## Compatibility

The matrix lists configured CI targets. A successful job qualifies that
source and build configuration; it does not cover every release in a series.

| NGINX | Commit checks | Extended checks |
|---|---|---|
| 1.30.5, 1.31.6 | All seven container suites | Same coverage |
| 1.31.6 | Strict GCC/Clang, incremental/pooled; ASan/UBSan; native macOS and Windows x64/x86 | Valgrind |
| 1.7.9 | Strict GCC, native helpers and runtime, incremental | Same coverage |
| 1.7.11, 1.7.12 | Strict GCC; epoll fallback / first corrected thread-pool notification | Same coverage |
| 1.20.2, 1.26.3, 1.28.3 | — | All seven container suites |
| 1.11.4, 1.11.5 | — | Strict GCC; manager callback seconds/milliseconds boundary |

Linux jobs use the same Docker images and test entry point as local runs.
Native macOS and Windows jobs exercise their own filesystem and process APIs;
Windows builds with MSVC and runs `nginx.exe` HTTP, path and reload checks.
The POSIX process/signal fixtures run on Linux and macOS. Windows does not
run those fixtures or the optional NGINX thread-pool path.

Thread support is optional. Builds without `--with-threads` use incremental
traversal; epoll builds before 1.7.12 also use that fallback to avoid a core
thread-notification race. BSD has no native CI job. Commit checks cover API
boundaries and current stable/mainline releases; scheduled, manual full and
release runs add historical representatives. Documentation-only changes skip
builds. See [testing](t/TESTING.md) for the exact triggers and commands.

The source is split into configuration, handlers, queue, walk, invalidation,
REFRESH and response components. Shared path helpers live in
`ngx_cache_purge.h`; platform operations use POSIX/Win32 headers. The former
`generation.*` unit is named `invalidation.*`. See the
[component map](docs/ARCHITECTURE.md).

The module follows NGINX's C89-compatible coding style and uses core event,
slab, rbtree and atomic APIs. It does not require C11 atomics or a newer
filesystem API. Strict GNU89 builds check declaration order and warnings;
the compiler and platform actually exercised are recorded with test results.

---

## Installation

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Dynamic module:

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

## Execution model

A **task** is one wildcard or `purge_all` operation. A **batch** is the number
of tasks a worker processes before a throttle pause. A **slice** is one
bounded step of an incremental directory walk; a task may need many slices.

With generation keys, one physical sweep per cache path and namespace coalesces all accepted invalidations. REFRESH pages are separate HTTP operations, not purge queue tasks.

| Request and configuration | File traversal | HTTP completion |
|---|---|---|
| Legacy exact key, with or without Vary matching | Request worker; Vary adds a directory walk | After the operation |
| Wildcard or `purge_all`, background queue off | Request worker, synchronous walk | After the walk |
| Background task, selected pool | One pool task per worker at a time | `202` after admission |
| Background task, no usable thread pool | Worker event loop, incremental slices | `202` after enqueueing |
| Queue capacity/allocation exhausted | Request waits for admission; no request-bound walk | `202` once admitted |
| Generation PURGE, any key shape | Deferred namespace sweep; no request-bound cache open/unlink | `200 invalidated` after publication and admission |
| REFRESH endpoint | Bounded scan and up to 32 serial HEAD probes | `200` JSON with completion and continuation fields |

Rows without generation keys describe legacy behavior. A generation request waiting for capacity returns `200` after admission, not `202`.

An incremental slice stops after at most 64 directory iterations or after
observing 5 ms of elapsed time. These budgets include skipped entries and
directory endings. They cannot bound a single filesystem syscall.

To move background file operations into threads, build with `--with-threads`
and define a core pool in the **main** context, outside `http`:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` selects an existing named pool in `http`,
`server` or `location`. Its default is `default`; NGINX creates that pool with
its standard defaults when it is referenced but not declared explicitly.
Custom pool names must be declared with core `thread_pool`. Pool selection is
inherited; the first accepted request selects the executor for coalesced work.
Each worker has its own core pools. Pool saturation retries submission after
10 ms and retains the task's admission credit; it never starts a synchronous
walk because the selected pool is full. Without compiled thread support,
on Windows, in single-process mode, or when an old task's named pool has
been removed by reload, execution uses the incremental cursor.

REFRESH uses the selected pool for one additional scanner per worker. Native HTTP subrequests run in the event loop; their state is never accessed by the I/O thread.

The queue and index use a slab mutex for structural edits. Admission and
background callbacks use trylock and retry through timers. Pending **and
running** tasks stay in a core string rbtree per cache path; comparisons of
complete keys resolve hash collisions. Index operations are `O(log Q)` for
`Q` outstanding tasks on that path; cache-path lookup is linear in the number
of registered paths. Native `ngx_queue_t` lists track pending and active work.
The first duplicate of a running operation requests a replay; further
requests coalesce into that replay. The replay goes to the pending tail after
the current pass, so a later purge is not silently discarded. Continuous
new purges may cause further passes. The queue credit is released only after
final completion. Filesystem work and response formatting run outside the lock.

The rbtree only deduplicates physical cleanup tasks. Generation rules live in bounded byte tries in a separate SHM zone. A strict namespace sweep has a different index identity from a legacy key task; REFRESH decisions join that same cleanup path.

A full queue or failed task allocation leaves the HTTP request waiting for
admission with a 10 ms retry timer and core disconnect handling. Requests and
cleanup handlers stay in their owning worker; SHM never contains request
pointers. HTTP/2 and HTTP/3 connection-wide reading is not disabled. No
429/503 is generated for queue pressure, and a short already-read PURGE is
not guaranteed to produce a TCP Zero Window. Outstanding requests still cost
request memory and connections; use core timeouts and admission policies for
the deployment's connection budget.

Accepted tasks retain their keys, pool names and temporary-path settings
across ordinary reloads. A newer duplicate updates the protection settings
for a pending task or its next replay. Disabling new background admission
retains accepted work. A gracefully exiting worker finishes its current pass
and takes no new task. Fast exit returns an unfinished incremental task to
the pending queue without completing its filesystem walk in the exit hook.
Core thread-pool teardown owns the lifetime of submitted thread tasks.

PID/slot epochs identify assigned tasks. Workers check at most 64 active
records per recovery pass, at one-second intervals, and requeue work whose
owner exited. This recovers abandoned assignments; it is not a transactional
journal. A crash during a shared-list or slab update can still corrupt state
and require a restart. Deletions performed immediately before a crash may be
absent from metrics. Pending work and replays need a running worker: drain
before stopping when completion matters. The queue is not durable.

Internal queue layouts are not an upgrade ABI. Deploy this build with a full
restart when replacing a build with a different layout, draining accepted
work first if it must complete. Ordinary reloads of the same build reuse the zone.

Deletion is best effort. A `202` confirms acceptance, not completed deletion;
a completed walk is not an atomic cache snapshot. Concurrent requests may
create cache entries during or after the walk, and file errors are logged.
Wildcard and full-cache walks unlink files without synchronously updating
every native cache node. Exact and Vary operations invalidate matched nodes.

Failed background passes retry after 100 ms, 1 s and 10 s, at most three times. Delayed tasks keep their deduplication/admission slot but allow other tasks to run. A fresh PURGE resets this retry window. After exhaustion the task is retired and an error is logged; inspect `cleanup.exhausted` and retry after fixing storage. Generation invalidation remains effective even if physical cleanup fails. Missing files are expected races; malformed files are skipped, not endlessly retried.

---

## Directives

### `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Inline form** (`from …`) — intercepts the named HTTP method on a proxy
location and purges the matching cache entry. `on` is a shorthand for method
`PURGE`; `off` disables purging. Optionally restrict to a list of CIDR ranges
or use `from all` to allow from any address. Adding `purge_all` before `from`
empties the entire cache zone regardless of the request URI.

**Separate-location form** (zone and key arguments) — allowed only inside a purge
location (typically a regex location capturing the cache key). Looks up the
cache zone by name and purges the compiled key expression. This form is
incompatible with `proxy_cache` and `proxy_pass` in the same location.

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for FastCGI cache zones configured with
`fastcgi_cache` / `fastcgi_cache_path`.

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for SCGI cache zones configured with
`scgi_cache` / `scgi_cache_path`.

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivalent to `proxy_cache_purge` but for uWSGI cache zones configured with
`uwsgi_cache` / `uwsgi_cache_path`.

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Selects the successful PURGE body format. `Cache-Purge-Code` and the body
result code agree; all module responses include `Cache-Control: no-store`.
Errors use `application/problem+json` (RFC 9457). Binary or nonrepresentable
keys use Base64 with an explicit encoding field; text keys are escaped.

`none` suppresses PURGE bodies: completed success becomes `204`, accepted work
stays `202`, and errors retain 4xx/5xx. Status and REFRESH keep structured JSON.
See the [HTTP contract](docs/HTTP_RESPONSES.md) for the catalog, fields,
examples and limits.

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Enables deferred wildcard and `purge_all` processing. A request returns
`202 Accepted` after enqueueing or coalescing with an identical pending or
running task. Exact-key purges are unaffected.

This switch controls legacy wildcard/all behavior. A `cache_purge_key` binding always uses deferred physical cleanup and logical `200` responses, even when this switch is off.

If the queue is full or a task allocation fails, the request stays open until
admission succeeds. It performs no directory walk while waiting. A duplicate
of an existing pending or running task can be accepted at capacity. Failure
to allocate the request's small wait state returns `500`. Disconnect cleanup
cancels the timer and releases the waiting gauge.

See [execution model](#execution-model) for threads, reload and shutdown.
For this flag and `cache_purge_vary_aware`, a repeated directive keeps the
last value. `cache_purge_legacy_status` is obsolete and ignored. Invalid flag values are rejected.

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maximum number of outstanding tasks across all cache paths. Both pending and
assigned tasks consume capacity. Duplicates reuse one allocation; a replay
keeps that allocation and credit. Distinct tasks wait for a free credit.

The shared zone is created when background processing or a status endpoint
is configured. Capacity changes require a restart, even when they round to
the same zone size. Reload rejects resizing to retain accepted work. Adding
cache roots can also require more zone memory and a restart. Drain pending
and active work first when it must complete.

Declaring a namespace also creates the task queue.

Zone size includes task storage, path metadata and aligned worker metrics.
It depends on the build and number of cache paths; see [memory sizing](#memory-sizing).

### `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Selects the executor for accepted background tasks, with normal inheritance.
For example:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX validates named pools and delivers completion through the native task
event. No private thread-pool layout or completion queue is copied. Custom
pool names must exist when threads are supported. Without thread support or
on Windows the directive is accepted and execution uses bounded event-loop
slices. A full selected pool delays execution; it does not move I/O back into
the event loop. Dedicated pools add thread stacks and context-switch costs.

### `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Maximum tasks processed by each worker between throttle pauses. Progress is
retained across traversal slices and thread completions. An empty queue ends
the current batch. Values above 64 are clamped with a configuration warning;
zero is rejected.

This controls tasks, not files or callback duration. Lower values introduce
more pauses between tasks; they do not interrupt an active threaded walk.
Use it together with `cache_purge_throttle_ms` when tuning disk load.

### `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Delay between background task batches. Accepts `10ms`, `500ms`, `1s` and
quoted compound values such as `"2s 500ms"`. A bare integer means milliseconds:
`10` and `10ms` are equivalent. Bare values produce a configuration warning;
an explicit unit avoids it.

An idle worker checks again after ten times this interval. Incomplete slices
and busy-mutex retries use 1 ms timers. Timer delays are minimum scheduling
intervals, not deadlines. Zero and values exceeding the platform's safe timer
range are rejected.

Admission, pool pressure and delayed cleanup retries use 10 ms polling.

### `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Serves a GET/HEAD JSON snapshot with `Cache-Control: no-store`. The path must
identify a configured cache root; unknown paths fail configuration loading,
even with background processing off. Restrict access to the endpoint:

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

`pending.full` counts full-cache operations. A wildcard whose key is `*` is
a separate operation type. `active` includes pool waiters; active tasks are
excluded from `queue.size` and `pending.total` but occupy admission capacity.

For a coalesced namespace sweep, `pending.full` reflects the first accepted task classification. REFRESH scans/probes are not included in the purge queue depth or deletion error counters. A rotated delayed retry can make `wait_time` a sampled head age rather than the maximum pending age.

Counters have one event-loop writer per process slot, with cache-line-aligned
starts and strides. Threads return results through completion events. The
status reader takes no queue mutex and performs no atomic fetch-add, including
zero increments. On 64-bit builds, counters use plain aligned volatile loads
and stores. On 32-bit builds, bounded sequence checks protect 64-bit reads;
a busy shard may be omitted from one sample. Fields are approximate and are
not a transaction across workers. Admission always uses locked control state,
never monitoring values.

Cumulative counters saturate at the unsigned 64-bit maximum, survive ordinary
reloads and reset on restart. The waiting gauge follows request cleanup.
Synchronous operations and core cache-manager deletions are excluded from
background deletion totals. Bytes are file-length estimates, not filesystem
blocks or proof of disk space reclaimed: open readers and concurrent writes
can change the result. Partial pass results are counted; crashed-worker results
can be missing. These metrics are not an audit log.

HIT/MISS observation counts only main requests with a native cache decision.
A missing upstream, cache status zero, BYPASS, EXPIRED, STALE, UPDATING and
REVALIDATED are excluded from this ratio. Ordinary uncached requests do not
become MISS. Monitoring alone can create the SHM zone even with background
purging off. On 32-bit systems, queue age covers intervals below about 24.9
days; a negative signed clock difference is reported as zero.

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Accepted for configuration migration only. Both values emit a warning and
have no effect. Remove the directive; invalid values still fail validation.
A synchronous miss returns `200 no_match`; accepted physical cleanup returns
`202 accepted`; published generation invalidation returns `200 invalidated`.
The former `412`/`404` switch is gone. See the [HTTP contract](docs/HTTP_RESPONSES.md).

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

When `on`, a successful exact-key purge walks the cache directory after deleting the
primary file and removes any remaining files that carry the same `KEY:` string.
This covers all `Vary` and `gzip_vary` variants of a cached response, which
are stored at different filesystem paths but share one logical key. The byte
immediately after the key string in each file is verified to be `\n`,
preventing false matches against keys that share only a common prefix.

Disabled by default because each exact purge adds a full directory walk. Enable it for multiple `Vary`/`gzip_vary` representations. The walk also runs when the primary entry is already absent. Wildcard and `purge_all` already cover variants. Exact Vary keys are case-sensitive and limited to 511 bytes; longer keys return `400 invalid_selector` before any deletion.

Generation invalidation covers every representation of the logical key without this option.

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Declares a generation namespace before locations bind it. Names are 1–32 ASCII letters, digits, `_` or `-`; SHM size must be at least eight native pages. Use a dedicated persistent state directory outside cache and temporary roots. The core creates a persistent exclusive marker to allocate a restart epoch. Retain these tiny marker files for as long as the corresponding cache may exist; deleting state while retaining cache can allow stale epoch reuse. No power-loss/fsync durability is promised. An ordinary reload preserves the epoch and rules; changing size/path or removing a namespace requires a full restart. Namespace SHM and the task queue have separate sizes.

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Binds the location to an existing namespace and exposes `$cache_purge_key`. Set the entire `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` or `uwsgi_cache_key` to that variable, including the corresponding separate PURGE location. A PURGE whose expanded native key differs from the generated key fails with `500` instead of claiming strict invalidation. Do not refer to `$cache_purge_key` inside its own logical expression. Logical keys have a maximum of 8096 bytes. Bind only participating locations when other locations need legacy semantics; there is no `off` form to cancel an inherited binding.

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Creates an explicit main-request-only REFRESH endpoint; other methods return `405`. It requires a `cache_purge_key` binding, `$cache_purge_key` as its native expression, and a refresh profile. The expanded logical key selects an exact key, a trailing-`*` prefix or `*` for the namespace in this cache path. Protect the location with core access/authentication rules. Only proxy caches support REFRESH; PURGE remains available for all four protocols.

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Maps one known representation to a literal internal proxy location. Removing the exact, case-sensitive prefix from the logical key must leave an origin URI beginning with `/`; query strings and escapes are retained. The module never guesses a host or searches a key for `://`. The target must have `internal`, cache off, request headers/body forwarding off, method HEAD, literal nonempty Host and literal representation headers. The two conditional headers must use exactly `$cache_purge_refresh_etag` and `$cache_purge_refresh_last_modified`. `$cache_purge_refresh_uri` supplies the mapped URI to `proxy_pass`. These three pure variables are meaningful only in probes. Target checks run in preaccess, including internal redirects. The administrator must ensure the mapping identifies the same origin representation as the cache key; Vary entries are skipped because native metadata does not retain their original request headers.

Set `proxy_intercept_errors off` in the probe location; inherited interception is rejected so an origin error cannot be rewritten into an invalidating success.

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Limits new scan/probe dispatch for a REFRESH page, including waiting for its worker scanner. Expiry stops new work and marks the response incomplete. An already-running HEAD finishes under core `proxy_connect_timeout`, `proxy_send_timeout` and `proxy_read_timeout`; configure those explicitly. This is not a hard response deadline and cannot interrupt a filesystem syscall. Disconnect and graceful shutdown stop further dispatch and retain an active thread's storage until native completion.

---

## Partial key purge

Append `*` to the **expanded cache key** for a case-insensitive prefix match. Exact-key and Vary matching remain case-sensitive. A physical prefix may contain up to 511 bytes, excluding the trailing `*`. A longer prefix is logged once and returns `400 invalid_selector` without walking files.

With `cache_purge_key`, matching uses the logical expression and accepts at most 8096 bytes including a trailing `*`.

For a key expression ending in `$uri`, this request selects a URI prefix:

```http
PURGE /images/header*
```

The asterisk must be the last byte of the evaluated key, not merely the last
byte of the URI path. Account for any query-string or variable suffix in
`proxy_cache_key`. A key consisting only of `*` walks every eligible entry.

---

## Generation invalidation

In a bound namespace, PURGE publishes a new generation and schedules a deduplicated stale-generation sweep before returning `200` with status `invalidated`. No cache-file open or unlink runs in that admission path. New requests that first evaluate the key after publication cannot hit the previous generation. A request pins its generation for each namespace/logical key, including internal redirects; an already-started response may finish, but its late cache write cannot become a hit for subsequent new requests. Exact matches are case-sensitive, prefixes are ASCII case-insensitive, and `purge_all` changes the whole namespace, including other cache paths that deliberately share it. Physical cleanup is queued for the addressed path; other paths use their core expiry or a subsequent purge.

An accepted invalidation does not promise immediate disk space reclamation. A late old write after a sweep is still unreachable and is removed by later cleanup or core inactive expiry. Under namespace SHM pressure the rules collapse to a new namespace baseline: additional keys become cold rather than invalidated keys reappearing. Reclamation processes at most 64 detached trie nodes per later invalidation. Repeated hot-key purges do not grow the rule tree indefinitely. Complexity depends on bounded key length and task-index size, not the number of cache files; a wall-clock O(1) or starvation-free guarantee under overload is not claimed. Queue admission may wait. After invalidation, use core `proxy_cache_lock` and equivalent cache controls to govern GET stampedes.

## Conditional REFRESH

REFRESH validates still-cached objects before their TTL expires; it neither prewarms them nor extends TTL on `304`. It complements core `proxy_cache_revalidate` and `proxy_cache_background_update`, which handle ordinary expired-object traffic. An origin that ignores validators and always sends `200` turns REFRESH into invalidation plus an extra HEAD. Only current, valid, canonical hash-path entries are candidates. A physical replacement in the same generation may be conservatively invalidated; generation comparison prevents an old HEAD from invalidating a later PURGE generation.

| Raw origin outcome | Action / result |
|---|---|
| `304` | Keep; `kept` |
| `200`, `404`, `410` | Conditional generation invalidation and deferred cleanup; `invalidated` |
| Other status | Keep; `upstream_status` |
| No origin status / transport failure | Keep; `transport_error` |
| Vary metadata | No HEAD; `skipped_vary` |
| Invalid mapping or profile | Keep; `skipped_profile` |
| Generation replaced meanwhile | No new invalidation; `raced_replaced` |
| Internal invalidation/allocation failure | Keep; `invalidation_error` |
| Work not dispatched/completed before the page stops | `undispatched`; cursor does not pass it |

One worker owns at most one active REFRESH page and one probe at a time; a shared budget allows at most 16 probes across workers. Other page requests wait without allocating a candidate array. Each page scans the directory with the common cursor, retains at most 32 candidates and then dispatches serial core HEAD subrequests. This bounds both candidate memory and the number of subrequests sharing a main-request pool. Identical REFRESH HTTP requests are not deduplicated; they share the concurrency limit. Their invalidations use the common deduplicated purge queue.

Start without a cursor, then send `X-Cache-Purge-Cursor: <next_cursor>` to the same endpoint with the same logical selector/profile until `complete` is true. The cursor is the last completed 32-hex filename; invalid or duplicate cursor headers return `400`. `scan_complete` means directory enumeration finished successfully; `complete` also means no further matching page and all selected decisions finished. `timed_out` and `scan_error` distinguish interruption/failure. `undispatched` counts selected entries not finished; an incomplete scan is only a lower bound, never a total cache count. If scanning is incomplete, no probes run, results are empty and the input cursor is returned unchanged. Results expose file hashes and raw upstream statuses, not keys or credentials.

Each page rescans the directory to select the next hashes, trading bounded memory for repeated I/O: `O(files × pages)` for a full operation. Pagination is weakly consistent under concurrent insertions/removals; restart from the beginning if a complete later snapshot is required. Choose selectors and timeouts accordingly. This administrative interface is unsuitable for repeatedly scanning very large live caches at high frequency.

### Generation and REFRESH example

```nginx
thread_pool purge_io threads=2 max_queue=32;
http {
    upstream origin { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx/objects levels=1:2 keys_zone=objects:10m;
    cache_purge_namespace objects 1m /var/lib/nginx/purge-epochs;
    cache_purge_thread_pool purge_io;
    cache_purge_response_type json;

    server {
        listen 8080;
        location /objects/ {
            cache_purge_key objects "origin.test$request_uri";
            proxy_cache objects;
            proxy_cache_key $cache_purge_key;
            proxy_cache_lock on;
            proxy_cache_valid 200 10m;
            proxy_set_header Host origin.test;
            proxy_pass http://origin;
            proxy_cache_purge PURGE from 127.0.0.1;
        }
        location = /admin/refresh {
            allow 127.0.0.1; deny all;
            cache_purge_key objects $http_x_cache_key;
            proxy_cache_refresh objects $cache_purge_key;
            cache_purge_refresh_profile /internal/cache-probe origin.test;
            cache_purge_refresh_timeout 10s;
        }
        location = /internal/cache-probe {
            internal;
            proxy_cache off;
            proxy_intercept_errors off;
            proxy_pass_request_headers off;
            proxy_pass_request_body off;
            proxy_connect_timeout 1s;
            proxy_send_timeout 2s;
            proxy_read_timeout 2s;
            proxy_method HEAD;
            proxy_set_header Host origin.test;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since $cache_purge_refresh_last_modified;
            proxy_pass http://origin$cache_purge_refresh_uri;
        }
    }
}
```

```sh
curl -X PURGE 'http://127.0.0.1:8080/objects/item'
curl -X REFRESH -H 'X-Cache-Key: origin.test/objects/*'      'http://127.0.0.1:8080/admin/refresh'
# Repeat with X-Cache-Purge-Cursor set to the returned next_cursor.
```

## Sample configurations

### Basic — inline purge method

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/images/logo.png
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match — no matching cache entry
# 403 — client IP not in the allowed list
```

### Basic — separate purge location

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass      http://backend;
            proxy_cache     main;
            proxy_cache_key "$host$uri$is_args$args";
        }

        # Capture the cache key from the URI and pass it to the zone/key form.
        # No proxy_pass or proxy_cache required in this location.
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

### Background queue enabled

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

### Purge all entries

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location /content {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/content/anything
# 200 — walk completed; inspect error_log for file errors
```

### JSON responses with per-location override

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        cache_purge_response_type json;   # default for all locations in this server
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location /admin/purge {
            allow 127.0.0.1;
            deny  all;
            # Override to plain text for scripting
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

### Vary-aware purge

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

After purging a key, all gzip and Vary variants stored at different filesystem
paths are also removed automatically.

---

## Performance tuning

The three controls address different limits: `queue_size` bounds outstanding
work, `batch_size` groups tasks per worker, and `throttle_ms` spaces those
groups. Measure request latency, disk contention and queue age before and
after changing them. More queue capacity does not increase drain throughput.

These presets are starting configurations, not hardware performance claims.
Column names omit the common `cache_purge_` prefix.

| Workload / storage budget | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Constrained host or storage | 512 | 5 | 25ms |
| General starting point | 2048 | 20 | 10ms |
| Fast storage with measured spare capacity | 8192 | 50 | 5ms |
| Short bursts with conservative background drain | 8192 | 5 | 50ms |

For example, the general preset is:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

A larger queue can absorb a burst, but sustained arrivals above completion
capacity will still fill it. Approximate burst headroom as
`max(0, arrival_rate - completion_rate) * burst_seconds` additional pending tasks,
using rates measured in tasks/s after duplicate coalescing. Leave headroom
for uneven task duration and existing backlog. Admission waiting adds open
requests and connections; account for their memory and FD budget when storage
is saturated.

### Memory sizing

The configured zone size is calculated from the native structure sizes:

```text
Q = cache_purge_queue_size; P = native page size
I = sizeof(ngx_http_cache_purge_queue_item_t)
H = sizeof(ngx_http_cache_purge_queue_t)
A = max(ngx_cacheline_size, 64)
R = align(sizeof(ngx_http_cache_purge_stats_t), A)
S = 2 * (I + 2 * (512 + 1))
M = sum over cache paths of
    2 * (sizeof(ngx_http_cache_purge_path_t) + path_length + 1
         + A + R * NGX_MAX_PROCESSES)
zone_bytes = max(8 * P, align(H + Q * S + M, P))
```

| Component | Reserved or used memory |
|---|---|
| Task reserve | `Q * S`, including allowance for slab rounding and inline key/pool names |
| Metrics for one path | `R * NGX_MAX_PROCESSES`, plus alignment; 192 KiB with a 192-byte stride and 1024 slots |
| Queue control | `H`, including process ownership and epoch arrays |
| Path reserve | `M`, including names, rbtree roots, alignment and metric arrays |
| Minimum zone | Eight native pages |

For the measured LP64 layout (`I=184`, `H=20576`, path record 104 bytes,
stats record 144 bytes), with a 64-byte cache line, 4 KiB pages and one
20-byte cache path, the formula gives:

| Capacity | Zone bytes | Zone MiB |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Actual task storage is `I + index_bytes + 1 + pool_name_bytes` before slab rounding. The embedded index contains a one-byte action tag plus the legacy key, or a namespace name for a generation sweep. Legacy `purge_all` stores only its tag. Capacity bounds pending and active allocations together. Native sizes, cache-line size, `NGX_MAX_PROCESSES` and configured paths determine the result; Windows and 32-bit layouts differ. Reload protection records consume additional SHM. Allocation may fail before the task limit.

Process memory is separate. Each active walk uses a fixed key buffer, one
reusable pathname buffer and a fixed-size frame per open directory. Frames
store prefix lengths without copying ancestor paths, so traversal storage is
`O(depth + longest_path)`, independent of file count. Directory streams have
platform buffers. Waiting requests hold a small timer/cleanup context, without
a traversal cursor. Thread stacks and core pools have separate memory costs.

Namespace SHM uses its separately configured size. One REFRESH scanner has a fixed 283864-byte LP64 candidate buffer/context (32 entries), plus traversal frames and at most 32 core subrequests in its HTTP request pool. Completing a page releases that request; continuation starts a new one. Additional waiters do not allocate candidate arrays.

### Throughput and latency

For a continuously busy worker, a useful approximation is:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Ignoring `D` gives the scheduling-only ceiling `1000 * B / T`: at defaults,
1000 tasks/s per worker. It is not a file-deletion rate or a measured result.
If ten tasks take 200 ms in total, the same 10 ms pause gives about 47.6
tasks/s. Workers share storage, so multiplying by worker count is not a
throughput guarantee. An idle worker may take up to
`10 * T` before checking for new work, plus event-loop delay.

Decrease batch size or increase the pause to reduce work between rest periods.
For lower worker latency during disk operations, use the optional pool.
Neither setting bounds one syscall or splits a threaded task into batches.

---

## Monitoring and debugging

Use the [status endpoint](#cache_purge_queue_status) to observe pending work,
active tasks, admission waiters, duplicates, unlink results and cache HIT/MISS.
`202 Accepted` confirms queue admission or coalescing; it does not prove deletion
or immediate logical invalidation. Monitor request latency alongside queue age
and storage contention.

| Level | Condition |
|---|---|
| `warn` | Excessive key length, removed task pool or clamped setting |
| `error` | Access denied, cache lookup or REFRESH profile failure, exhausted cleanup retries; core pool or Windows read failure |
| `crit` | File open/stat/deletion or directory operation failure; POSIX core read failure |
| `alert` | File close failure |
| `emerg` | Invalid configuration, incompatible SHM, resizing or reload metadata allocation failure |
| `debug` | Additional request and traversal context |

Module diagnostics use native NGINX logging functions and start with
`ngx_cache_purge: `. NGINX supplies severity, timestamp, process/thread IDs,
errno text and request or configuration context for the configured output.
Filesystem messages identify the actual failed operation and quote its path;
namespace and REFRESH failures identify the affected object and rejection
reason. Use a standard NGINX error-log parser and filter the message prefix.

Debug messages use `NGX_LOG_DEBUG_HTTP` and require both a `--with-debug`
build and `error_log /var/log/nginx/error.log debug;`.

Queue pressure is represented by admission-wait metrics instead of one
warning per retry. An already vanished
file is an expected race and does not produce a module diagnostic. Messages
from NGINX core helpers, including the configuration parser, allocator, I/O
and thread pools, retain the core format; inspect the full log when diagnosing
failures.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

See [logging checks](t/TESTING.md#logging-contracts) for tested diagnostics.

---

## Troubleshooting

| Symptom | Check or adjustment |
|---|---|
| Worker latency during large walks | Enable the background queue; use the optional pool for disk I/O. Exact/Vary and background-off operations remain synchronous. |
| Admission waiters grow | Check arrival rate, queue age, pool capacity and disk load. Extra task capacity absorbs bursts; resizing requires restart. |
| High iowait | Reduce `cache_purge_batch_size`, increase `cache_purge_throttle_ms`, and limit incoming purges. |
| Queue drains slowly | Measure task duration and storage contention before increasing batch size or reducing the pause. |
| Clients expect the former `412`/`404` miss status | Remove `cache_purge_legacy_status`; handle `200` with `Cache-Purge-Code: no_match`. |
| Vary variants remain | Enable `cache_purge_vary_aware`; check the primary purge result and the 511-byte variant-key limit. |
| Accepted work appears incomplete | Check active tasks, recovery and file errors. `202` is not a completion receipt; suspected SHM corruption requires a restart. |

---

## Testing

Build and run the complete suite in Docker:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

From `t/`, the existing Compose services remain available:

```sh
docker-compose -f docker-compose.test.yml build nginx-test
docker-compose -f docker-compose.test.yml run --rm nginx-test

docker-compose -f docker-compose.test.yml run --rm test-basic
docker-compose -f docker-compose.test.yml run --rm test-queue
docker-compose -f docker-compose.test.yml run --rm test-config
docker-compose -f docker-compose.test.yml run --rm test-memory
docker-compose -f docker-compose.test.yml run --rm test-performance
docker-compose -f docker-compose.test.yml run --rm test-native
docker-compose -f docker-compose.test.yml run --rm test-runtime
```

Use `docker compose` in place of `docker-compose` with the Compose plugin.
The Makefiles detect either command. `nginx-test` and `make test-all` run
all seven `t/*.t` suites through the same `prove` entry point used in CI.
The integration image includes native helper/layout checks and pooled runtime
checks. Strict builds without threads exercise incremental runtime execution.
Each failed test fails the command.

See [`t/TESTING.md`](t/TESTING.md) for strict builds, sanitizers, Valgrind,
platform jobs and reports. All GitHub jobs are defined in `ci.yml`.

---

## Migration

### Queue and status changes

Deploy changes to the shared-memory layout with a full restart. Existing
method/ACL syntax and protocol handlers remain available. HTTP behavior
changes in v4: JSON is the default, result codes are shared by headers/bodies,
misses return `200 no_match`, and `cache_purge_legacy_status` warns and is ignored.
Update clients using the [HTTP migration notes](docs/HTTP_RESPONSES.md#migration-from-v2v3). Queue capacity now includes assigned tasks; overload waits
for admission and then returns `202`, instead of running a request-bound walk.
Active duplicates may schedule a replay in the same task allocation.

Status adds fields while retaining existing field names. `requests.overflow`
now counts admission waiters. Metrics are approximate and use 64-bit cumulative
counters. A threaded build automatically registers `default` when referenced;
use a named pool to isolate purge I/O. Status-only configurations allocate SHM.

Generation invalidation and REFRESH are explicit opt-ins in v4.0. Bind `cache_purge_key` and use `$cache_purge_key` as the entire native cache key in every participating location. This changes physical filenames; existing legacy files are cold and remain for core expiry or an explicit legacy cleanup. Keep state markers with the cache. Existing PURGE locations do not gain REFRESH. Users of experimental PR60 must configure the new endpoint/profile and paginate; PR60's unbounded response, guessed URI/Host mapping and implicit method expansion are not compatibility promises.

### From the original FRiCKLE module

The method/access-list and zone/key forms remain available. Missing entries
return `200 no_match`. Remove `cache_purge_legacy_status` and update clients
for the [v4 HTTP contract](docs/HTTP_RESPONSES.md). To opt in to background processing, add:

```nginx
http {
    cache_purge_background_queue on;
}
```

### From nginx-selective-cache-purge-module

Replace the module. Background queue functionality is now built in. Map your
previous queue parameters to `cache_purge_queue_size`, `cache_purge_batch_size`,
and `cache_purge_throttle_ms`.

---

## Temporary directories

Walks skip configured temporary directories nested below the cache root,
using complete path-component comparisons. Names such as `proxy_temp` are
not reserved when they are not configured temporary paths. Only regular
files with exactly 32 hexadecimal filename characters are deletion candidates;
numeric and suffixed cache-write temporary files are skipped, including
writes with `use_temp_path=off`.

Every walk also validates the native header version, offsets, complete `KEY: ` marker, CRC32 and key/variant filename hash with a fixed 8192-byte key limit. Malformed, truncated, foreign-version or mismatched files are skipped even by purge-all. ETag/Vary lengths equal to the core maximum are accepted. Header validation detects format errors; it does not authenticate a malicious file.

The configured cache root may be a symlink or Windows junction. Descendant symlinks/reparse points are skipped. On POSIX with the complete descriptor-relative feature probe, child directories and files use core `ngx_openat_file`/`ngx_file_at_info` with no-follow flags and deletion is relative to the held parent directory. This prevents redirection through a replaced descendant symlink; it is not a sandbox against moving an already-open directory. Windows and older Unix use core full-path operations with link checks and retain a path-replacement race. Legacy exact-key core lookup/deletion also uses full paths. Keep cache roots private and do not allow untrusted writers.

Use consistent canonical spelling for configured paths. Comparisons do not
resolve symlink, mount or Unicode aliases. Queued tasks retain their protection set; a newer duplicate updates pending
work or the next replay. A running pass keeps its existing set. This is not
a union of all live worker generations. Do not repurpose paths while affected tasks are pending
or active.

---

## Security

- Restrict purge endpoints with `allow` / `deny` directives or an upstream
  authentication layer. Never expose them publicly.
- Set `cache_purge_queue_size` to a value appropriate for your traffic to
  limit shared memory consumption under request floods.
- Consider pairing purge locations with `limit_req` to rate-limit purge
  requests independently of normal traffic.

---

## License

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denis Denisov

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

---

## See also

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)
