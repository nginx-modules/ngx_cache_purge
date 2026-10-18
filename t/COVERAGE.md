# What the v4 tests cover

The suite checks the main v4 workflows, but it does not prove complete coverage.
There is no collected line/branch coverage percentage. A passing helper test is
not the same as a passing HTTP request through a real NGINX worker. A green CI
run covers only the versions, build options and cases selected for that run.

The missing-ETag runtime cases expose an upstream NGINX zero-length `memcpy`
nonnull diagnostic under UBSan, recorded as a narrowly scoped TODO. See
[sanitizer details](TESTING.md#additional-v4-regression-cases).

Use this map when deciding which checks a change needs. Commands and the CI
matrix are in [the testing guide](TESTING.md).

| Behavior | Existing evidence | Limits |
|---|---|---|
| Exact, prefix and full PURGE | `basic.t`, `background_queue.t`, `purge_runtime.t`: real cache files, HIT/MISS, missing entries, Vary and both configuration forms | Proxy has the broadest runtime coverage. FastCGI/SCGI/uWSGI now exercise real local origins, native cache fills, exact/prefix PURGE and generation keys; their full Vary/reload/failure matrix is not duplicated |
| Generation keys | `generation_main.c`: exact/prefix/namespace rules, selector bounds, allocation pressure, reclamation and counter saturation; `purge_runtime.t`: late writes, shared namespaces across two cache paths, reload, restart epoch and deferred deletion | Power-loss behavior is not qualified; fsync durability is not promised |
| Native cache policies | `purge_runtime.t`: lock owner/timeout/age, 503 stale fallback, minimum-use warmup and bypass/no-store separation for all four upstream protocols; proxy 304 revalidation and background updating across PURGE | These are selected interactions, not the complete directive cross-product; see the [directive map](PERFORMANCE.md#directive-coverage-map) |
| Vary publication | Proxy tests populate 66 representations across two axes in legacy and generation modes, repeat after reload, retain case/prefix neighbors, and finish a held body after generation cleanup | Gateway-specific Vary, changing axes, `Vary: *` and index-loss behavior need separate scenarios; v4 has no variant index |
| Queue and overload | `queue_state_main.c`, `batch_scheduler_main.c`, `purge_runtime.t`: pending/running deduplication, replay, capacity, waiting, disconnect and batch pauses | No live multi-stream HTTP/2 or HTTP/3 pressure/disconnect test |
| Retry and worker lifecycle | Helper-injected allocation/submission/scan failures; live reload, graceful exit and worker termination in `purge_runtime.t` | Killing a process during every possible slab/list mutation is not tested; the queue is not a durable or crash-transactional journal |
| File safety | `cache_entry_main.c`, directory/path fixtures and runtime walks: header layout, key/CRC/hash bounds, short reads, temporary paths, links and replacement races | Real filesystems, mount types, permission failures and storage faults are not an exhaustive matrix; BSD has no native CI job |
| REFRESH decisions | `purge_runtime.t`: weak ETag, date-only/both/missing validators, unchanged file metadata on 304, 200/404/410, redirects, origin errors, transport failure, Vary, URI/query mapping, late-probe races, deadlines, disconnect and more than 32 entries; `refresh_completion_main.c`: terminal callback state | The production credit helper is tested at the 16-probe boundary, under lock contention, on release and dead/orphan-owner recovery. Sustained multiworker saturation, disconnect during an active scan and directory churn during pagination still need stress tests |
| REFRESH profile isolation | `purge_runtime.t`: in-flight refill versus a ready candidate; dynamic/empty Host, forwarding of headers/body, cache on, error interception, wrong method, public target, missing validator expressions, dynamic representation headers and internal redirects | Tests assert that rejected profiles send no origin HEAD and leave a HIT usable; correct origin/representation mapping remains the administrator’s responsibility |
| HTTP result contract | `basic.t`, `config.t`, `purge_runtime.t`: JSON defaults, ignored legacy flags, `200`/`202`/`204`, problem details, header/body agreement, escaping, binary keys, method/access errors, malformed/duplicate cursors and live PURGE/REFRESH scan faults | Not every terminal result has a live fault-injection test. In particular, `delete_failed` and `internal_error` still lack live HTTP fault-injection cases |
| Builds and platforms | Strict GNU89 GCC/Clang and feature guards; real core metadata layouts; native macOS; MSVC x86/x64 build and selected Windows runtime cases | No dynamic-module load/ABI matrix, native BSD run, or claim that every intermediate NGINX release is tested |
| Memory and performance | ASan/UBSan; Linux LSan controls; optional extended Valgrind; memory/load smoke suites; manual [Actions experiment](PERFORMANCE.md) with real cache files, raw responsiveness probes, worker snapshots and ABI/stack reports | Measurements exist only after a run. Closed-loop probes do not establish offered-load tail latency; no sustained RSS/FD ceiling, multi-day soak or production throughput guarantee |

Paths ending in `.c` above refer to [test fixtures](fixtures/).
The corresponding [native helper runner](native_helpers.t) compiles
production helper bodies with controlled inputs; it does not replace the live
[runtime suite](purge_runtime.t).

## Interpreting failures

A failed image build means its tests did not run. A failed `CI` gate can be the
consequence of one earlier job; it is not a second module defect. Read the first
failing assertion and its prerequisites before changing production behavior.

For example, the REFRESH profile test must first establish a current cache
entry. After a `410`-driven invalidation, one completed GET is not proof that
NGINX has finished publishing its cache file. An empty REFRESH result then says
there was no current candidate; it says nothing about profile validation.
The tests now wait for HIT and exercise blocked refill separately from unsafe
profiles. They retain the expected rejection and diagnostic checks.

## Remaining qualification work

Before relying on a specific untested deployment path, add an observable
end-to-end case for it. The highest-value gaps are HTTP/2/3 concurrency, the
missing HTTP error cases, protocol-specific Vary/reload failures, REFRESH budget
and scan-cancellation stress, and dynamic-module loading. Storage-specific soak
and failure testing belongs to the target deployment as well.

The cache-manager callback fixture is a compatibility prototype. It does not
mean v4 has a production tag index or a cache-loader indexing backend; those
features are not implemented and are not included in this coverage map.
