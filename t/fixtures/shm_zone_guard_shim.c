/*
 * Shim for init_shm_zone() and traffic_class_bind(): the zone mutex, the log,
 * the slab allocator and the two helpers that need a full nginx cycle.
 */
static unsigned locked, locks, errors, slab_calls, protection_calls;
static ngx_uint_t last_level;

static void test_log(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{
    (void) log; (void) err; (void) fmt;
    last_level = level;
    errors++;
}
#undef ngx_log_error
#define ngx_log_error test_log

static void test_lock(ngx_shmtx_t *mutex)
{
    (void) mutex;
    assert(!locked);
    locked = 1;
    locks++;
}

static void test_unlock(ngx_shmtx_t *mutex)
{
    (void) mutex;
    assert(locked);
    locked = 0;
}
#define ngx_shmtx_lock test_lock
#define ngx_shmtx_unlock test_unlock

/* ngx_slab_calloc() hands out zeroed memory; the queue relies on that. */
static void *test_slab_calloc(ngx_slab_pool_t *pool, size_t size)
{
    (void) pool;
    slab_calls++;
    return calloc(1, size);
}
#define ngx_slab_calloc test_slab_calloc

static ngx_http_cache_purge_protection_t  protection_stub;

static ngx_http_cache_purge_protection_t *
ngx_http_cache_purge_protection_get(ngx_http_cache_purge_queue_t *queue,
    ngx_array_t *paths)
{
    (void) queue; (void) paths;
    protection_calls++;
    return &protection_stub;
}

static ngx_int_t
ngx_http_cache_purge_bind_queue_status(ngx_http_cache_purge_main_conf_t *cmcf)
{
    (void) cmcf;
    return NGX_OK;
}
