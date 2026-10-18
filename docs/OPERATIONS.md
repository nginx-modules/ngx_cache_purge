# Operating the cache purge module

Configuration reference: [directives](DIRECTIVES.md).

<a id="generation-invalidation"></a>

## Generation-key invalidation

Use generation keys when newly arriving requests must stop using stale content as soon as PURGE completes. A request already fetching from the origin may finish later, but its old cache write cannot become a HIT for new requests. Physical files are deleted separately.

A namespace groups keys that share invalidation rules. Keep its state directory with the cache across restarts. Sharing a namespace across cache paths shares invalidation, while cleanup only targets the addressed path. See [performance tuning](#performance-tuning) before deployment.

<a id="reading-cache-files"></a>

### Reading cache files in generation mode

A tool that reads cache files itself (a cache browser, a preloader, a usage report) cannot tell stale files from live ones by looking at the directory: after an invalidation both exist until cleanup removes the old ones, and a key that was invalidated but not yet requested again has only a stale file. The `KEY:` line of a generation file is `p4:<namespace>:<epoch>:<generation>:<logical key>`, and the [status endpoint](DIRECTIVES.md#cache_purge_queue_status) reports the namespace state needed to judge it. A file is stale when its epoch differs from the live epoch or its generation is below the live generation of its logical key; this is the test the cleanup applies. The live generation of a key is `baseline`, unless an exact or prefix invalidation raised it, and the status reply does not list those rules.

| Status of the file's namespace | Verdict |
|---|---|
| Namespace missing from the list | `unknown` |
| File epoch differs from `epoch` | `stale` |
| Epoch matches; file generation is below `baseline` | `stale` |
| Epoch matches; generation is at least `baseline`; `rules` is `false` | `live` |
| Epoch matches; generation is at least `baseline`; `rules` is `true` | `unknown` |

Here, **live** means not stale by namespace generation; it does not establish freshness, TTL validity or an NGINX cache HIT. `namespaces_complete=false` leaves returned entries usable and makes omitted namespaces unknown.

An unknown file can still be settled from the files alone in one case: if another file has the same namespace, epoch and logical key and a higher generation, the lower one is stale, because the live generation of a key never decreases within an epoch. Treat everything else as unknown; do not guess live or stale.

Read status before and after the scan, matching entries by namespace name. Use status-based verdicts for a namespace only if it appears in both replies and `epoch`, `baseline` and `serial` are unchanged. Missing or changed state invalidates that namespace's verdicts. Compare epochs only for equality. These observations do not prevent later invalidations.

Do not use an idle cleanup queue as a replacement for these verdicts. A request that began before an invalidation can still write an old-generation file after the cleanup of that invalidation has finished, and that file stays until a later cleanup or native cache eviction. Cleanup also addresses only the cache path of the purge location, while a namespace can be shared by several cache paths.

<a id="performance-tuning"></a>

## Performance tuning

The three controls address different limits: `queue_size` bounds outstanding work, `batch_size` groups tasks per worker, and `throttle_ms` spaces those groups. Measure request latency, disk contention and queue age before and after changing them. More queue capacity does not increase drain throughput.

These presets are starting configurations, not hardware performance claims. Column names omit the common `cache_purge_` prefix.

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

A larger queue can absorb a burst, but sustained arrivals above completion capacity will still fill it. Approximate burst headroom as `max(0, arrival_rate - completion_rate) * burst_seconds` additional pending tasks, using rates measured in tasks/s after duplicate coalescing. Leave headroom for uneven task duration and existing backlog. Admission waiting adds open requests and connections; account for their memory and FD budget when storage is saturated.

### Memory sizing

Queue capacity counts pending and assigned tasks together. The queue zone also needs per-path metadata and per-worker metrics, so its size depends on the build, cache paths and native structure sizes. Allocation can fail before the task limit. Namespace memory is configured separately.

Measure the target build and leave room for waiting requests, traversal buffers, thread stacks and REFRESH pages in process memory. The queue sizing calculation is documented beside its implementation in [`ngx_cache_purge_init_main_conf()`](../ngx_cache_purge_module.c).

### Throughput and latency

For a continuously busy worker, a useful approximation is:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Ignoring `D` gives the scheduling-only ceiling `1000 * B / T`: at defaults, 1000 tasks/s per worker. It is not a file-deletion rate or a measured result. If ten tasks take 200 ms in total, the same 10 ms pause gives about 47.6 tasks/s. Workers share storage, so multiplying by worker count is not a throughput guarantee. An idle worker may take up to `10 * T` before checking for new work, plus event-loop delay.

Decrease batch size or increase the pause to reduce work between rest periods. For lower worker latency during disk operations, use the optional pool. Neither setting bounds one syscall or splits a threaded task into batches.

<a id="monitoring-and-debugging"></a>

## Monitoring and debugging

Use the [status endpoint](DIRECTIVES.md#cache_purge_queue_status) to observe pending work, active tasks, admission waiters, duplicates, unlink results and cache HIT/MISS. `202 Accepted` confirms queue admission or coalescing; it does not prove deletion or immediate logical invalidation. Monitor request latency alongside queue age and storage contention.

| Level | Condition |
|---|---|
| `warn` | Excessive key length, removed task pool or clamped setting |
| `error` | Access denied, cache lookup or REFRESH profile failure, exhausted cleanup retries; core pool or Windows read failure |
| `crit` | File open/stat/deletion or directory operation failure; POSIX core read failure |
| `alert` | File close failure |
| `emerg` | Invalid configuration, incompatible SHM, resizing or reload metadata allocation failure |
| `debug` | Additional request and traversal context |

Module diagnostics use native NGINX logging functions and start with `ngx_cache_purge: `. NGINX supplies severity, timestamp, process/thread IDs, errno text and request or configuration context for the configured output. Filesystem messages identify the actual failed operation and quote its path; namespace and REFRESH failures identify the affected object and rejection reason. Use a standard NGINX error-log parser and filter the message prefix.

Debug messages use `NGX_LOG_DEBUG_HTTP` and require both a `--with-debug` build and `error_log /var/log/nginx/error.log debug;`.

Queue pressure is represented by admission-wait metrics instead of one warning per retry. An already vanished file is an expected race and does not produce a module diagnostic. Messages from NGINX core helpers, including the configuration parser, allocator, I/O and thread pools, retain the core format; inspect the full log when diagnosing failures.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

See [logging checks](../t/TESTING.md#logging-contracts) for tested diagnostics.

<a id="troubleshooting"></a>

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

<a id="temporary-directories"></a>

## Temporary directories

Walks skip configured temporary directories nested below the cache root, using complete path-component comparisons. Names such as `proxy_temp` are not reserved when they are not configured temporary paths. Only regular files with exactly 32 hexadecimal filename characters are deletion candidates; numeric and suffixed cache-write temporary files are skipped, including writes with `use_temp_path=off`.

Every walk also validates the native header version, offsets, complete `KEY: ` marker, CRC32 and key/variant filename hash with a fixed 8192-byte key limit. Malformed, truncated, foreign-version or mismatched files are skipped even by purge-all. ETag/Vary lengths equal to the core maximum are accepted. Header validation detects format errors; it does not authenticate a malicious file.

The configured cache root may be a symlink or Windows junction. Descendant symlinks/reparse points are skipped. On POSIX with the complete descriptor-relative feature probe, child directories and files use core `ngx_openat_file`/`ngx_file_at_info` with no-follow flags and deletion is relative to the held parent directory. This prevents redirection through a replaced descendant symlink; it is not a sandbox against moving an already-open directory. Windows and older Unix use core full-path operations with link checks and retain a path-replacement race. Legacy exact-key core lookup/deletion also uses full paths. Keep cache roots private and do not allow untrusted writers.

Use consistent canonical spelling for configured paths. Comparisons do not resolve symlink, mount or Unicode aliases. Queued tasks retain their protection set; a newer duplicate updates pending work or the next replay. A running pass keeps its existing set. This is not a union of all live worker generations. Do not repurpose paths while affected tasks are pending or active.

<a id="security"></a>

## Security

- Restrict purge endpoints with `allow` / `deny` directives or an upstream authentication layer. Never expose them publicly.
- Set `cache_purge_queue_size` to a value appropriate for your traffic to limit shared memory consumption under request floods.
- Consider pairing purge locations with `limit_req` to rate-limit purge requests independently of normal traffic.
