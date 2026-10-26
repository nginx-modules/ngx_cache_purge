/*
 * The queue zone outlives a reload, so a module built from other sources can
 * meet a zone laid out by a different build (a dynamic module replaced on disk
 * and reloaded). Two properties keep that safe, and neither is otherwise
 * tested:
 *
 *  1. Every change of a structure that lives in the zone comes with a new
 *     NGX_CACHE_PURGE_SHM_VERSION. The compiler's layout for the version in
 *     the header must equal the fingerprint recorded here for that version.
 *  2. init_shm_zone() compares the version before it reads or writes anything
 *     only the current layout has (class_n, class_names). A refused zone is
 *     left byte for byte as it was: the previous generation still uses it.
 *
 * The refusal checks do not depend on a sanitizer, but with ASan the older
 * zone is an allocation of exactly its old size, so a read past it also fails.
 */

#if (NGX_PTR_SIZE == 8 && !NGX_WIN32 && NGX_MAX_PROCESSES == 1024)
#define HAVE_FINGERPRINT 1

/*
 * version, sizeof queue, path, stats, queue item, metric count, and the
 * indexes of NGX_PURGE_HIT and NGX_PURGE_EXHAUSTED. The same numbers come out
 * of NGINX 1.20.2, 1.26.3 and 1.31.7 on LP64 Linux.
 *
 * Changed a zone structure on purpose? Raise NGX_CACHE_PURGE_SHM_VERSION and
 * append the row printed by a failing run; never edit the row of a version that
 * has been released.
 */
static const struct {
    int     version;
    size_t  queue, path, stats, item;
    int     metrics, hit, exhausted;
} known[] = {
    { 7, 20576, 104, 144, 184, 17, 11, 16 },
    { 8, 20688, 104, 192, 184, 23, 11, 16 }
};

static void
check_layout(void)
{
    unsigned  i;
    int       found;

    found = 0;
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (known[i].version != (int) NGX_CACHE_PURGE_SHM_VERSION) {
            continue;
        }
        found = 1;
        if (known[i].queue != sizeof(ngx_http_cache_purge_queue_t)
            || known[i].path != sizeof(ngx_http_cache_purge_path_t)
            || known[i].stats != sizeof(ngx_http_cache_purge_stats_t)
            || known[i].item != sizeof(ngx_http_cache_purge_queue_item_t)
            || known[i].metrics != (int) NGX_PURGE_METRICS
            || known[i].hit != (int) NGX_PURGE_HIT
            || known[i].exhausted != (int) NGX_PURGE_EXHAUSTED)
        {
            fprintf(stderr, "the shared zone layout changed without a new "
                    "NGX_CACHE_PURGE_SHM_VERSION (%d); a zone laid out by the "
                    "previous build would be misread. Current layout:\n"
                    "    { %d, %lu, %lu, %lu, %lu, %d, %d, %d }\n",
                    (int) NGX_CACHE_PURGE_SHM_VERSION,
                    (int) NGX_CACHE_PURGE_SHM_VERSION,
                    (unsigned long) sizeof(ngx_http_cache_purge_queue_t),
                    (unsigned long) sizeof(ngx_http_cache_purge_path_t),
                    (unsigned long) sizeof(ngx_http_cache_purge_stats_t),
                    (unsigned long) sizeof(ngx_http_cache_purge_queue_item_t),
                    (int) NGX_PURGE_METRICS, (int) NGX_PURGE_HIT,
                    (int) NGX_PURGE_EXHAUSTED);
            abort();
        }
    }
    if (!found) {
        fprintf(stderr, "no layout fingerprint for NGX_CACHE_PURGE_SHM_VERSION "
                "%d: add its row to the table\n",
                (int) NGX_CACHE_PURGE_SHM_VERSION);
        abort();
    }
}
#endif

static ngx_http_cache_purge_main_conf_t  conf, old_conf;
static ngx_slab_pool_t                   pool;
static ngx_log_t                         zone_log;
static ngx_shm_zone_t                    zone;

/* A new cycle's configuration: two class names, poisoned slots. */
static void
reset(void)
{
    ngx_uint_t  i;

    memset(&conf, 0, sizeof(conf));
    memset(&old_conf, 0, sizeof(old_conf));
    memset(&zone, 0, sizeof(zone));
    memset(&pool, 0, sizeof(pool));
    conf.queue_size = old_conf.queue_size = 16;
    conf.traffic_class_n = 2;
    conf.traffic_class_names[0].data = (u_char *) "bot";
    conf.traffic_class_names[0].len = 3;
    conf.traffic_class_names[1].data = (u_char *) "human";
    conf.traffic_class_names[1].len = 5;
    for (i = 0; i < NGX_CACHE_PURGE_CLASSES; i++) {
        conf.traffic_class_slot[i] = 99;    /* bind must not have run */
    }
    ngx_str_set(&zone.shm.name, "cache_purge_zone");
    zone.data = &conf;
    zone.shm.addr = (u_char *) &pool;
    zone.shm.log = &zone_log;
    locked = locks = errors = slab_calls = protection_calls = 0;
}

/* A zone of another version: exactly `size` bytes, arbitrary contents. */
static ngx_http_cache_purge_queue_t *
other_zone(ngx_uint_t version, size_t size)
{
    ngx_http_cache_purge_queue_t  *queue;

    queue = malloc(size);
    assert(queue != NULL);
    memset(queue, 0xA5, size);
    queue->version = version;
    queue->shpool = &pool;
    return queue;
}

/* init_shm_zone() refuses: logged once, nothing locked, bound or written. */
static void
assert_refused(ngx_int_t rc, const void *before, const void *zone_now,
    size_t size)
{
    assert(rc == NGX_ERROR);
    assert(errors == 1 && last_level == NGX_LOG_EMERG);
    assert(locks == 0 && !locked);
    assert(protection_calls == 0 && slab_calls == 0);
    assert(conf.queue == NULL);
    assert(conf.traffic_class_slot[0] == 99 && conf.traffic_class_slot[1] == 99);
    assert(memcmp(before, zone_now, size) == 0);
}

int main(void)
{
    ngx_http_cache_purge_queue_t  *older, *newer, *queue;
    void                          *snapshot;
    size_t                         older_size, newer_size;

#if (HAVE_FINGERPRINT)
    check_layout();
#else
    printf("shm layout fingerprint: skipped (needs LP64 Unix)\n");
#endif

    /* What a build without the class slots allocated for its queue. */
    older_size = offsetof(ngx_http_cache_purge_queue_t, class_n);
    newer_size = sizeof(ngx_http_cache_purge_queue_t);
    assert(older_size < newer_size);

    /* Reload against a zone of the previous version. */
    older = other_zone(NGX_CACHE_PURGE_SHM_VERSION - 1, older_size);
    snapshot = malloc(older_size);
    assert(snapshot != NULL);
    memcpy(snapshot, older, older_size);
    reset();
    old_conf.queue = older;
    assert_refused(ngx_http_cache_purge_init_shm_zone(&zone, &old_conf),
                   snapshot, older, older_size);

    /* Windows worker attaching to a zone the master laid out differently. */
    reset();
    zone.shm.exists = 1;
    pool.data = older;
    assert_refused(ngx_http_cache_purge_init_shm_zone(&zone, NULL),
                   snapshot, older, older_size);
    free(snapshot);
    free(older);

    /* A zone of a newer version is refused as well (a downgrade). */
    newer = other_zone(NGX_CACHE_PURGE_SHM_VERSION + 1, newer_size);
    snapshot = malloc(newer_size);
    assert(snapshot != NULL);
    memcpy(snapshot, newer, newer_size);
    reset();
    old_conf.queue = newer;
    assert_refused(ngx_http_cache_purge_init_shm_zone(&zone, &old_conf),
                   snapshot, newer, newer_size);
    free(snapshot);
    free(newer);

    /* An old configuration that never attached a queue. */
    reset();
    old_conf.queue = NULL;
    assert(ngx_http_cache_purge_init_shm_zone(&zone, &old_conf) == NGX_ERROR);
    assert(errors == 1 && locks == 0 && conf.queue == NULL);

    /*
     * Control: the same entry point accepts a zone of the current version, so
     * the refusals above are not an artefact of the harness.
     */
    reset();
    assert(ngx_http_cache_purge_init_shm_zone(&zone, NULL) == NGX_OK);
    assert(errors == 0 && !locked && slab_calls == 1 && protection_calls == 1);
    queue = conf.queue;
    assert(queue != NULL && pool.data == queue);
    assert(queue->version == NGX_CACHE_PURGE_SHM_VERSION);
    assert(queue->max_size == 16);
    assert(conf.traffic_class_slot[0] == 0 && conf.traffic_class_slot[1] == 1);
    assert(queue->class_n == 0);    /* checked here, stored by init_module */
    assert(ngx_http_cache_purge_traffic_class_commit(&conf, &zone_log) == NGX_OK);
    assert(queue->class_n == 2);

    /* ... and a reload that finds it again binds the names to the same slots. */
    reset();
    conf.traffic_class_names[0].data = (u_char *) "human";
    conf.traffic_class_names[0].len = 5;
    conf.traffic_class_names[1].data = (u_char *) "bot";
    conf.traffic_class_names[1].len = 3;
    old_conf.queue = queue;
    assert(ngx_http_cache_purge_init_shm_zone(&zone, &old_conf) == NGX_OK);
    assert(errors == 0 && conf.queue == queue && slab_calls == 0);
    assert(conf.traffic_class_slot[0] == 1 && conf.traffic_class_slot[1] == 0);
    assert(queue->class_n == 2);

    assert(!locked);
    free(queue);
    return 0;
}
