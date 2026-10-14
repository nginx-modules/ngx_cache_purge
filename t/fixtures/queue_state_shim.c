static ngx_http_cache_purge_work_t ngx_cache_purge_work;
ngx_module_t ngx_http_cache_purge_module;
volatile ngx_msec_t ngx_current_msec;
ngx_uint_t ngx_exiting;
sig_atomic_t ngx_terminate, ngx_quit;
static unsigned locked, live, fail_alloc, yield_walk, dead_owner, scan_errors;
static ngx_uint_t ngx_cache_purge_slot;
ngx_uint_t ngx_cacheline_size = 64;
ngx_pid_t ngx_pid = 100;
#if (NGX_THREADS)
static unsigned posts, reject_post;
#endif

static void test_log(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{
    (void) level; (void) log; (void) err; (void) fmt;
}
#undef ngx_log_error
#undef ngx_log_debug2
#undef ngx_log_debug3
#define ngx_log_error test_log
#define ngx_log_debug2 test_log
#define ngx_log_debug3 test_log

static void test_lock(ngx_shmtx_t *mutex)
{
    (void) mutex;
    assert(!locked);
    locked = 1;
}
static void test_unlock(ngx_shmtx_t *mutex)
{
    (void) mutex;
    assert(locked);
    locked = 0;
}
static ngx_uint_t test_trylock(ngx_shmtx_t *mutex)
{
    test_lock(mutex);
    return 1;
}
#define ngx_shmtx_lock test_lock
#define ngx_shmtx_unlock test_unlock
#define ngx_shmtx_trylock test_trylock

static void *test_alloc(ngx_slab_pool_t *pool, size_t size)
{
    void *p;
    (void) pool;
    assert(locked);
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    p = calloc(1, size);
    assert(p != NULL);
    live++;
    return p;
}
static void test_free(ngx_slab_pool_t *pool, void *p)
{
    (void) pool;
    assert(locked && live && p != NULL);
    live--;
    free(p);
}
#define ngx_slab_calloc_locked test_alloc
#define ngx_slab_free_locked test_free

/* Force equal hashes so the production index must compare complete keys. */
static uint32_t test_hash(u_char *data, size_t len)
{
    assert(data != NULL);
    (void) len;
    return 7;
}
#define ngx_crc32_short test_hash

ngx_int_t ngx_http_cache_purge_generation_invalidate(
    ngx_http_cache_purge_namespace_t *ns, ngx_str_t *key, ngx_flag_t all, ngx_str_t *expected)
{
    (void) ns; (void) key; (void) all; (void) expected;
    assert(locked);
    return NGX_OK;
}

static void ngx_http_cache_purge_walk_init(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *root,
    ngx_http_cache_purge_protection_t *protection, ngx_array_t *paths,
    ngx_log_t *log)
{
    (void) protection; (void) paths; (void) log;
    assert(!locked);
    memset(ctx, 0, sizeof(*ctx));
    ctx->cache_root = *root;
}
static ngx_int_t ngx_http_cache_purge_walk_step(
    ngx_http_cache_purge_cursor_t *cursor, ngx_uint_t limit)
{
    (void) limit;
    assert(!locked);
    cursor->started = 1;
    cursor->ctx->scan_errors = scan_errors;
    return yield_walk ? NGX_AGAIN : NGX_OK;
}

#if (NGX_THREADS)
static ngx_int_t test_post(ngx_thread_pool_t *pool, ngx_thread_task_t *task)
{
    assert(!locked && pool != NULL && !task->event.active);
    posts++;
    if (reject_post) { task->event.active = reject_post == 2; return NGX_ERROR; }
    task->event.active = 1;
    return NGX_OK;
}
static void test_timer(ngx_event_t *event, ngx_msec_t delay)
{
    (void) event; (void) delay;
}
#define ngx_thread_task_post test_post
#undef ngx_add_timer
#define ngx_add_timer test_timer
#endif

static ngx_uint_t ngx_http_cache_purge_process_alive(ngx_pid_t pid)
{ (void) pid; assert(locked); return !dead_owner; }
static void ngx_http_cache_purge_walk_close(ngx_http_cache_purge_cursor_t *cursor)
{ cursor->top = NULL; }
#if (NGX_THREADS)
static ngx_thread_pool_t *test_pool_get(ngx_cycle_t *cycle, ngx_str_t *name)
{ (void) cycle; assert(name->len == 7); return ngx_cache_purge_work.pool; }
#define ngx_thread_pool_get test_pool_get
#endif
