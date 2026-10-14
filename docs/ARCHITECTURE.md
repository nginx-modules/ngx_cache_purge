# Module components

The build still produces one NGINX module. Its implementation is separated
by responsibility, with sources listed explicitly in `config`.

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

“Generation” names the version of a logical cache key/namespace. Advancing
that version makes older entries unreachable, including late writes using
an old key. It describes one mechanism, not all v4 features. The former
`ngx_cache_purge_generation.*` files are therefore named `invalidation.*`;
internal generation terminology remains where it describes that mechanism.

The private headers are not a third-party ABI. Splitting files does not
remove dependencies on version-specific NGINX cache and protocol structures;
native layout checks and boundary builds cover those dependencies.

## Execution ownership

Workers own HTTP requests and completion callbacks. The shared queue owns
accepted physical cleanup tasks independently of the submitting request.
Optional core thread pools perform filesystem work; event-loop callbacks
publish results and release admission credits. Incremental execution uses
bounded traversal steps when no usable pool is available. REFRESH performs
HTTP subrequests only in the event loop.

NGINX 1.7.11 introduced thread pools, but its `epoll` notification writes the
wakeup before installing the callback. A worker can observe a null callback.
The ordering is corrected in NGINX 1.7.12. On affected earlier epoll builds,
this module warns and uses incremental execution for purge/REFRESH I/O.
This fallback does not repair other users of the old core thread-pool API.
The compatibility matrix checks both 1.7.11 fallback and 1.7.12 pooled paths.

## Native cache loader

The current design has no filesystem tag index to rebuild at cold start.
Generation rules and namespace epochs do not require discovering every cache
file before serving requests. Reusing the native cache loader therefore
does not replace an existing bootstrap pass in this implementation.

For a future persistent file-to-tag index, populating it during the native
loader walk could avoid a second directory walk. That needs a defined metadata
format and a core integration point: `ngx_http_file_cache_loader` is an internal
callback, not a public per-file module hook. Merely wrapping the path's loader
callback does not expose every visited file. Any integration must preserve
the native manager/loader data and lifecycle, including the manager callback
unit change at 1.11.5. The fixture for that boundary is a prototype, not a
production loader backend.

Relevant core implementation: [`src/http/ngx_http_file_cache.c`](https://github.com/nginx/nginx/blob/master/src/http/ngx_http_file_cache.c),
[`src/event/modules/ngx_epoll_module.c`](https://github.com/nginx/nginx/blob/master/src/event/modules/ngx_epoll_module.c),
and the versioned [1.7.11](https://nginx.org/download/nginx-1.7.11.tar.gz) /
[1.7.12](https://nginx.org/download/nginx-1.7.12.tar.gz) source archives.
