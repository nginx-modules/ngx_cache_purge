# Migrating to v4

## Queue and status changes

Deploy changes to the shared-memory layout with a full restart. Existing method/ACL syntax and protocol handlers remain available. HTTP behavior changes in v4: JSON is the default, result codes are shared by headers/bodies, misses return `200 no_match`, and `cache_purge_legacy_status` warns and is ignored. Update clients using the [HTTP migration notes](HTTP_RESPONSES.md#migration-from-v2v3). Queue capacity now includes assigned tasks; overload waits for admission and then returns `202`, instead of running a request-bound walk. Active duplicates may schedule a replay in the same task allocation.

Status adds fields while retaining existing field names. `requests.overflow` now counts admission waiters. Metrics are approximate and use 64-bit cumulative counters. A threaded build automatically registers `default` when referenced; use a named pool to isolate purge I/O. Status-only configurations allocate SHM.

Generation invalidation and REFRESH are explicit opt-ins in v4.0. Bind `cache_purge_key` and use `$cache_purge_key` as the entire native cache key in every participating location. This changes physical filenames; existing legacy files are cold and remain for core expiry or an explicit legacy cleanup. Keep state markers with the cache. Existing PURGE locations do not gain REFRESH.

## From the original FRiCKLE module

The method/access-list and zone/key forms remain available. Missing entries return `200 no_match`. Remove `cache_purge_legacy_status` and update clients for the [v4 HTTP contract](HTTP_RESPONSES.md). To opt in to background processing, add:

```nginx
http {
    cache_purge_background_queue on;
}
```

## From nginx-selective-cache-purge-module

Replace the module. Background queue functionality is now built in. Map your previous queue parameters to `cache_purge_queue_size`, `cache_purge_batch_size`, and `cache_purge_throttle_ms`.
