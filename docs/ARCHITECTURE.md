# Module components

The build still produces one NGINX module. Its implementation is separated by responsibility, with sources listed explicitly in `config`.

| File | Responsibility |
|---|---|
| `ngx_cache_purge_module.c` | Module registration, directives, configuration and lifecycle wiring |
| `ngx_cache_purge_handler.c` | HTTP dispatch, access checks, protocol/cache lookup and exact purge |
| `ngx_cache_purge_queue.c` | SHM admission and deduplication, scheduling, worker lifecycle and status metrics |
| `ngx_cache_purge_walk.c` | Bounded directory traversal, native cache-header validation, unlink and Vary cleanup |
| `ngx_cache_purge_invalidation.c` | Namespace state, generation rules, cache-key binding and invalidation |
| `ngx_cache_purge_refresh.c` | Bounded scans, conditional HEAD subrequests and continuation |
| `ngx_cache_purge_response.c` | HTTP result catalog, headers, escaping and response bodies |
| `ngx_cache_purge.h` | Private shared types/contracts and common path helpers |
| `ngx_cache_purge_invalidation.h` | Private invalidation interface |
| `ngx_cache_purge_upstream.h` | Protocol interfaces and version-specific core layout adapters |
| `ngx_cache_purge_posix.h`, `ngx_cache_purge_win32.h` | Platform filesystem operations |

“Generation” names the version of a logical cache key/namespace. Advancing that version makes older entries unreachable, including late writes using an old key. It describes one mechanism, not all v4 features. The invalidation component owns this mechanism.

The private headers are not a third-party ABI. Splitting files does not remove dependencies on version-specific NGINX cache and protocol structures; native layout checks and boundary builds cover those dependencies.

## Execution ownership

Workers own HTTP requests and completion callbacks. The shared queue owns accepted physical cleanup tasks independently of the submitting request. Optional core thread pools perform filesystem work; event-loop callbacks publish results and release admission credits. Incremental execution uses bounded traversal steps when no usable pool is available. REFRESH performs HTTP subrequests only in the event loop.

NGINX 1.7.11 introduced thread pools, but its `epoll` notification writes the wakeup before installing the callback. A worker can observe a null callback. The ordering is corrected in NGINX 1.7.12. On affected earlier epoll builds, this module warns and uses incremental execution for purge/REFRESH I/O. This fallback does not repair other users of the old core thread-pool API. The compatibility matrix checks both 1.7.11 fallback and 1.7.12 pooled paths.

## Native cache loader

The current design has no filesystem tag index to rebuild at cold start. Generation rules and namespace epochs do not require discovering every cache file before serving requests. Reusing the native cache loader therefore does not replace an existing bootstrap pass in this implementation.

For a future persistent file-to-tag index, populating it during the native loader walk could avoid a second directory walk. That needs a defined metadata format and a core integration point: `ngx_http_file_cache_loader` is an internal callback, not a public per-file module hook. Merely wrapping the path's loader callback does not expose every visited file. Any integration must preserve the native manager/loader data and lifecycle, including the manager callback unit change at 1.11.5. The fixture for that boundary is a prototype, not a production loader backend.

Relevant core implementation: [`src/http/ngx_http_file_cache.c`](https://github.com/nginx/nginx/blob/master/src/http/ngx_http_file_cache.c), [`src/event/modules/ngx_epoll_module.c`](https://github.com/nginx/nginx/blob/master/src/event/modules/ngx_epoll_module.c), and the versioned [1.7.11](https://nginx.org/download/nginx-1.7.11.tar.gz) / [1.7.12](https://nginx.org/download/nginx-1.7.12.tar.gz) source archives.

## Execution model

In this section, **invalidation** means making an old cache key unusable; **cleanup** means deleting its files. **Admission** means reserving a queue slot for pending or active cleanup. **SHM** is memory shared by NGINX workers; a **worker** is a process serving requests. A namespace sweep is a directory walk that removes files made obsolete by generation rules.

A **task** is one wildcard or `purge_all` operation. A **batch** is the number of tasks a worker processes before a throttle pause. A **slice** is one bounded step of an incremental directory walk; a task may need many slices.

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

An incremental slice stops after at most 64 directory iterations or after observing 5 ms of elapsed time. These budgets include skipped entries and directory endings. They cannot bound a single filesystem syscall.

To move background file operations into threads, build with `--with-threads` and define a core pool in the **main** context, outside `http`:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` selects an existing named pool in `http`, `server` or `location`. Its default is `default`; NGINX creates that pool with its standard defaults when it is referenced but not declared explicitly. Custom pool names must be declared with core `thread_pool`. Pool selection is inherited; the first accepted request selects the executor for coalesced work. Each worker has its own core pools. Pool saturation retries submission after 10 ms and retains the task's admission credit; it never starts a synchronous walk because the selected pool is full. Without compiled thread support, on Windows, in single-process mode, or when an old task's named pool has been removed by reload, execution uses the incremental cursor.

REFRESH uses the selected pool for one additional scanner per worker. Native HTTP subrequests run in the event loop; their state is never accessed by the I/O thread.

The queue and index use a slab mutex for structural edits. Admission and background callbacks use trylock and retry through timers. Pending **and running** tasks stay in a core string rbtree per cache path; comparisons of complete keys resolve hash collisions. Index operations are `O(log Q)` for `Q` outstanding tasks on that path; cache-path lookup is linear in the number of registered paths. Native `ngx_queue_t` lists track pending and active work. The first duplicate of a running operation requests a replay; further requests coalesce into that replay. The replay goes to the pending tail after the current pass, so a later purge is not silently discarded. Continuous new purges may cause further passes. The queue credit is released only after final completion. Filesystem work and response formatting run outside the lock.

The rbtree only deduplicates physical cleanup tasks. Generation rules live in bounded byte tries in a separate SHM zone. A strict namespace sweep has a different index identity from a legacy key task; REFRESH decisions join that same cleanup path.

A full queue or failed task allocation leaves the HTTP request waiting for admission with a 10 ms retry timer and core disconnect handling. Requests and cleanup handlers stay in their owning worker; SHM never contains request pointers. HTTP/2 and HTTP/3 connection-wide reading is not disabled. No 429/503 is generated for queue pressure, and a short already-read PURGE is not guaranteed to produce a TCP Zero Window. Outstanding requests still cost request memory and connections; use core timeouts and admission policies for the deployment's connection budget.

Accepted tasks retain their keys, pool names and temporary-path settings across ordinary reloads. A newer duplicate updates the protection settings for a pending task or its next replay. Disabling new background admission retains accepted work. A gracefully exiting worker finishes its current pass and takes no new task. Fast exit returns an unfinished incremental task to the pending queue without completing its filesystem walk in the exit hook. Core thread-pool teardown owns the lifetime of submitted thread tasks.

PID/slot epochs identify assigned tasks. Workers check at most 64 active records per recovery pass, at one-second intervals, and requeue work whose owner exited. This recovers abandoned assignments; it is not a transactional journal. A crash during a shared-list or slab update can still corrupt state and require a restart. Deletions performed immediately before a crash may be absent from metrics. Pending work and replays need a running worker: drain before stopping when completion matters. The queue is not durable.

Internal queue layouts are not an upgrade ABI. Deploy this build with a full restart when replacing a build with a different layout, draining accepted work first if it must complete. Ordinary reloads of the same build reuse the zone.

Deletion is best effort. A `202` confirms acceptance, not completed deletion; a completed walk is not an atomic cache snapshot. Concurrent requests may create cache entries during or after the walk, and file errors are logged. Wildcard and full-cache walks unlink files without synchronously updating every native cache node. Exact and Vary operations invalidate matched nodes.

Failed background passes retry after 100 ms, 1 s and 10 s, at most three times. Delayed tasks keep their deduplication/admission slot but allow other tasks to run. A fresh PURGE resets this retry window. After exhaustion the task is retired and an error is logged; inspect `cleanup.exhausted` and retry after fixing storage. Generation invalidation remains effective even if physical cleanup fails. Missing files are expected races; malformed files are skipped, not endlessly retried.
