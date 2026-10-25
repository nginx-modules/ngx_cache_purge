/* Shim for traffic_class_bind(): it needs only the zone mutex and the log. */
static unsigned locked, locks, errors;
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
