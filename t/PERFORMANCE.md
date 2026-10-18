# Cache correctness and performance qualification

This review starts from v4 commit `9c6c6d7` and the NGINX 1.31.6 source archive.
It adds regression scenarios and an opt-in measurement workflow. It does not
change production invalidation or claim measured speedups. Results require an
actual Actions run; adding a test is not evidence that it passes.

## Run through GitHub Actions

Commit the patch and let the existing build/runtime jobs finish. The new cases
are part of `purge_runtime.t`, including its existing compatibility, thread,
sanitizer and macOS lanes. They need no separate service or paid runner.

Once `.github/workflows/cache-benchmark.yml` exists on the default branch, open
**Actions > Cache purge benchmark > Run workflow** and select the tested branch.
Start with 1,000 unrelated files, 2 variants, `compare`, 5 trials and one worker.
Then use 10,000 / 100,000 files and 16 / 66 variants as needed. One million files
is an explicit large experiment: capacity checks may reject it, and the run
may exceed its time budget. Use one mode at a time for the largest population.
Changing the worker count or directory layout is a separate experiment.

The workflow uses one standard Ubuntu runner and runs only on manual dispatch.
There is a 30-minute experiment limit inside a 45-minute job limit. It uploads
logs and partial reports on failure, retains artifacts for seven days, and
does not create a release or contact an external origin. Cache files live on a
runner bind mount, outside Docker's writable overlay. Container networking is
disabled; both HTTP endpoints use its loopback interface.

The optimized images contain two binaries from the same core and module source:
one without thread support, and one with `--with-threads`. Merely omitting the
pool directive from a threaded build would still allow the default pool and
would not select incremental execution.

| Mode | Execution | Meaning of the measured HTTP acknowledgement |
|---|---|---|
| `legacy-vary` | Exact PURGE plus synchronous Vary directory walk | Physical operation completed for the files it observed |
| `generation` | Generation publication, deferred incremental namespace walk | Logical invalidation published and cleanup admitted |
| `generation-threads` | Same publication, cleanup in a named two-thread pool | Same logical contract; disk operations run in pool threads |

Compare acknowledgement and physical cleanup separately. A ratio between the
first and second rows' acknowledgement times is not a like-for-like deletion
speedup. Threads can improve worker responsiveness without reducing total I/O.

### Workload and measurements

`cache_benchmark.py` fills real native cache files through HTTP. Eight bounded
producers populate unrelated keys; the target has two Vary axes and 2, 16 or 66
representations. Every target is observed as a HIT before measurement. The
initial core cache-loader process must exit before the trials begin.

Before each trial the harness checks the native-file population and records
the old target filenames. This reads file headers and metadata, so all reported
trials are **warm after validation**, including trial 1. It does not drop the
page cache or claim a cold-storage measurement. Validation time is excluded.

Each trial records:

- PURGE acknowledgement time, observed disappearance of all old target files,
  and observed queue drain. Deletion/drain observations are polled at about
  20 ms, with HTTP and scheduling overhead; they are upper-bound observations,
  not exact unlink timestamps.
- Raw timings of alternating uncached `/ping` and unrelated cache-HIT requests,
  with a two-second baseline before PURGE. A single closed-loop client opens a
  fresh connection per probe and pauses 5 ms. Slow replies reduce sample count;
  these probes reveal stalls but do not measure an offered-load tail-latency
  distribution. Phase summaries include probes overlapping a phase boundary.
- Worker CPU ticks, I/O counters, PSS/RSS and FD counts before PURGE and after
  cleanup. These snapshots include monitoring work, do not measure peaks, and
  do not include origin refill after the trial. Linux memory fields are in KiB;
  I/O counters retain `/proc` units. Shared mappings must not be counted once
  per worker by summing RSS.

The harness checks response bodies, every replacement's initial MISS and later
HIT, unrelated HIT survival, cleanup error counters, worker identity, error logs
and clean shutdown. Refills start after old filenames disappear because legacy
refills reuse those names. Partial `results.json` contains only trials whose
replacement checks completed; the final summary requires all selected modes.

Artifacts contain `purges.csv`, per-mode `results.json`, raw `probes-*.csv`,
configuration, logs and `environment.json`. Quantiles include sample counts;
p99 is omitted below 100 samples. No latency threshold makes the job green or
red. Repeat runs and inspect raw distributions before claiming an improvement.
Runner storage, CPU scheduling and filesystem caches vary between runs.

`incremental/layout.json` and `threads/layout.json` report actual configured
`sizeof` values, page size and compiler/core configuration. `stack-usage.tsv`
contains GCC's per-function stack estimates from `-fstack-usage`. Neither
report measures allocator fragmentation, recursive call-chain stack peaks,
thread-stack commitment, total process memory or an allocation count.

## Native cache model that the tests exercise

Source basis: [NGINX 1.31.6](https://nginx.org/download/nginx-1.31.6.tar.gz),
archive SHA-256
`974ed5298a5e398e008704ed5db284e655fc270c596493dbccada452448fc9f1`.
The following conclusions come from its implementation, not from directive
names alone. Older supported cores still require their compatibility lanes.

| Core path | Consequence for invalidation and tests |
|---|---|
| `ngx_http_upstream_cache()` and `ngx_http_file_cache_open()` | Lookup uses the native key/hash and selects HIT, expired, updating or miss behavior. A generation change selects a different native key. |
| `ngx_http_file_cache_lock()` and its wait handler | The lock belongs to a native cache node; it does not serialize PURGE. A timeout allows a non-cacheable fetch, while lock age can allow another filler. Test late owners and concurrent old fillers. |
| Vary handling in `ngx_http_file_cache.c` | Native primary/secondary hashes represent variants. The generation prefix belongs to their common logical identity. Test multiple axes, more than 64 variants, exact boundaries and reload. |
| `ngx_http_upstream_send_response()` and `ngx_http_file_cache_update()` | Response headers precede body completion and cache-file publication. Seeing a header filter run does not establish that a file was successfully published. Hold a body across PURGE and cleanup. |
| Stale fallback and `ngx_http_upstream_cache_background_update()` | Stale policy needs an existing candidate for that native key. A background subrequest can outlive the response it served. Assert actual STALE/UPDATING behavior before testing generation isolation. |
| 304 handling and `ngx_http_file_cache_update_header()` | Revalidation can finish after invalidation and touch the old file. Its response and file identity must not affect the replacement generation. |
| `ngx_http_internal_redirect()` and `ngx_http_subrequest()` | Redirects clear module contexts; subrequests share the request pool but have distinct request identities. Generation snapshots cannot simply be moved to one ordinary `r->ctx` slot. |

The module retains generation snapshots with pool-cleanup records matched to
the request pointer. An in-flight request may finish with old content. The
contract is that newly arriving requests use the new generation; PURGE does
not revoke bytes already being sent to a client.

The new stale tests expire real native files using an offset and width obtained
from the configured core header. They then require a real STALE response, so a
test cannot pass by accidentally exercising only a MISS. Controlled errors
carry `Cache-Control: no-store` to keep recovery assertions independent of
negative-response caching.

### Directive coverage map

The inventory below comes from the command tables in the proxy, FastCGI, SCGI
and uWSGI modules. `*` means all four prefixes except where explicitly stated.
It describes scenarios present in the suite, not an assertion that every
directive combination or version has passed.

| Directive or native option | Scenario present | Further qualification |
|---|---|---|
| `*_cache`, `*_cache_key` | Real fills and exact/prefix generation PURGE for all four protocols; shared namespace across paths and key validation | Variable cache-zone selection and more internal-redirect combinations |
| `*_cache_path` | Native hashed paths, temporary-path exclusion, file/header checks and lifecycle tests; benchmark selects flat, `1:2` or `2:2` | Eviction under `max_size`, `min_free` and `inactive`; concurrent loader/manager pressure; full `use_temp_path` matrix |
| `*_cache_lock` | Blocked old lock owner cannot hold a new generation, all four protocols | Waiter cancellation, many contenders and worker death while owning a native lock |
| `*_cache_lock_timeout` | Second old request reaches origin after timeout, PURGE occurs before both old replies finish | Boundary values and sustained timeout pressure |
| `*_cache_lock_age` | Second filler reaches origin after lock age, both old writers finish after PURGE | Repeated ownership turnover and wakeup races |
| `*_cache_use_stale` | Real HTTP 503 stale fallback before PURGE and no old stale fallback afterwards, all four protocols | Transport failure, timeout, invalid header, other supported status selectors and `off` |
| `*_cache_background_update` | Proxy STALE/UPDATING, blocked updater, cleanup, late old file and replacement HIT | Other protocols, disconnect and repeated updater failure; skip cores without this directive |
| `*_cache_revalidate` | Proxy conditional GET, actual 304 after PURGE, replacement body survives | Other protocols and date-only revalidation; REFRESH HEAD tests are a separate mechanism |
| `*_cache_bypass`, `*_no_cache` | Separate read and write policies across PURGE for all four protocols | Multiple predicates, empty/zero values and authenticated cache-key isolation |
| `*_cache_min_uses` | New generation repeats pre-threshold misses and eventually becomes a HIT, all four protocols | Concurrent fills close to the threshold |
| `*_cache_valid` | Normal 200 fills and deliberately expired native entries | Negative caching, `any`, expiration boundaries and header-driven validity precedence |
| `*_cache_methods` | Ordinary GET fills are exercised | Explicit HEAD/POST methods, method-bearing logical keys and body-dependent keys |
| `*_cache_max_range_offset` | No dedicated interaction scenario | Range beyond threshold, partial replies, PURGE during fill and slice-key composition |
| `proxy_cache_convert_head` | No dedicated interaction scenario; proxy only | HEAD conversion on/off with a method-aware key and mixed GET/HEAD fills |

Also qualify `*_ignore_headers` and the response headers that influence cache
decisions: `Cache-Control` (including stale extensions), `Expires`,
`X-Accel-Expires`, `Set-Cookie` and `Vary` (including `*` and changing axes).
Request Authorization/Cookie handling, key construction, buffering, client
disconnect policy and slice configuration need explicit deployment cases.
Do not assume a user-defined logical key isolates representations or methods
that it omits. Range and HEAD controls are not available on every historical
core; guard future tests against the actual configured source/features.

## Full scans and cleanup optimization

Current v4 has no per-key file index. Legacy exact PURGE with
`cache_purge_vary_aware on` still scans the cache directory synchronously.
Generation invalidation moves the correctness decision out of that walk and
coalesces physical cleanup by path and namespace, but physical cleanup remains
linear in the files visited. An incremental slice bounds work between yields,
not the duration of a single filesystem syscall. Queue size cannot increase
the storage device's drain capacity, and admission can delay acknowledgement.

A future file index should accelerate physical cleanup while generation keys
continue to provide logical invalidation. Incomplete state after eviction,
reload, worker failure or a late write must retain a defined fallback.
Observing response headers is insufficient evidence of a completed cache
write. Reusing the native loader also requires an actual per-file integration
point; wrapping its callback does not provide one.

Before adding an index, measure how much work coalescing already avoids and
whether latency comes from synchronous traversal, storage contention, or
namespace-lock contention. The manual experiment isolates a single target;
it does not measure a continuous purge stream or admission saturation. Those
are separate experiments, alongside simultaneous exact/prefix/full requests,
many namespaces, reload, and loader/manager activity.

## Memory and data-structure review

The v3.0.4 walk used a fixed 512-byte key buffer in a stack context, avoiding
per-file pool growth. Its synchronous full walk could still occupy a worker.
That historical tradeoff does not make a stack-only design suitable for state
that must survive an event-loop yield or thread completion.

Current v4 already reuses one 8193-byte key buffer per walk. The cursor grows a
single path buffer geometrically and allocates a frame per open directory,
not per visited file. Its space scales with depth and longest path, plus libc
directory buffers. Open directory handles also carry the no-follow traversal
identity, so replacing them with bare path strings changes safety properties.

| Component | Cost visible in source | Candidate and required evidence |
|---|---|---|
| Namespace rule tries | One node per distinct byte along separate exact/prefix tries; three pointers, a generation and a byte per node. On a conventional LP64 ABI this is 40 raw bytes and a 64-byte slab class, not 40 committed bytes. | A compressed radix representation could save SHM for long low-sharing keys. Measure rule cardinality, branch fanout, lookup time under the namespace mutex and slab pressure first. Preserve prefix normalization, full-key equality and conservative invalidation on allocation failure. |
| Rule reclamation | Namespace reset detaches live rules; later invalidations reclaim at most 64 garbage nodes each. | Measure repeated pressure/reset cycles and retained garbage. Immediate unbounded freeing under the namespace mutex would lengthen every blocked cache request. |
| Per-path metrics | One aligned record per `NGX_MAX_PROCESSES` slot. With a 144-byte record, 64-byte alignment and 1024 slots, the array is 192 KiB per path; sizing reserves additional reload metadata. | Sparse slot allocation or a redesigned record could reduce cost. Preserve historical counters, old/new worker overlap, alignment and 32-bit reader behavior. Current worker count is not a safe array bound. |
| REFRESH candidates | 32 entries each reserve an 8193-byte key array: 262176 bytes for those arrays alone, before native headers and scanner state. | Compact short keys into a bounded arena or allocate only used candidates. Prove ownership through scan completion, pagination, cancellation and partial allocation failure. Do not borrow a walk buffer after its visitor returns. |
| Waiting REFRESH requests | Each creates a default 16-KiB pool before obtaining its scanner; the large candidate array is deferred. | A smaller initial pool or lazy scanner-owned storage is a useful first prototype. Stress waiting requests, disconnect and active-thread cancellation; HTTP request state still costs memory. |
| Request generation snapshots | Small pool allocations for request state, snapshot and encoded key; logical bytes share the encoded buffer. | Coallocating the snapshot and encoded key may save bookkeeping, but small pool allocation usually bumps a pointer rather than calling malloc. Preserve redirects, request identity and large-key allocation thresholds. |
| Reload protection sets | Identical settings are reused; distinct retained settings consume additional SHM. | Reclamation needs ownership across old worker cycles, assigned work and replay. An empty queue alone does not prove old workers have released a set. |

Core files relevant to those decisions are `src/core/ngx_palloc.c`,
`ngx_slab.c`, `ngx_cycle.c`, `ngx_thread_pool.c`,
`src/os/unix/ngx_process_cycle.c` and `src/http/ngx_http_request.c`.
Pool reset does not execute cleanup handlers. Thread work and its completion
event must outlive the caller, and graceful reload overlaps worker generations.
Changing a SHM layout requires an explicit compatibility/version decision and
restart procedure, not just a smaller C struct.

Measure the existing layout first. Prototype REFRESH waiting/candidate storage
separately from trie or metrics redesign, then compare the same Actions presets
and run the relevant lifecycle/sanitizer tests. This separates memory savings
from changes to invalidation semantics and makes regressions attributable.
