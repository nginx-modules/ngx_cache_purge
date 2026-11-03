/*
 * cache_purge_traffic_class: a class name keeps the zone slot it was first
 * given; a configuration that needs more slots than are free is refused whole
 * and leaves the zone untouched. See traffic_class_bind() in queue.c.
 */
static ngx_http_cache_purge_queue_t      queue;
static ngx_http_cache_purge_main_conf_t  conf;
static ngx_slab_pool_t                   pool;
static ngx_log_t                         zone_log;
static ngx_uint_t                        commit_mode = 1;

static ngx_int_t
bind_names(const char *a, const char *b, const char *c)
{
    const char  *names[3];
    ngx_uint_t   n, i;

    names[0] = a;
    names[1] = b;
    names[2] = c;
    for (n = 0; n < 3 && names[n] != NULL; n++) {
        conf.traffic_class_names[n].data = (u_char *) names[n];
        conf.traffic_class_names[n].len = strlen(names[n]);
    }
    conf.traffic_class_n = n;
    for (i = 0; i < NGX_CACHE_PURGE_CLASSES; i++) {
        conf.traffic_class_slot[i] = 99;   /* poison: bind must set what it uses */
    }
    return ngx_http_cache_purge_traffic_class_bind(&queue, &conf, &zone_log,
                                                   commit_mode);
}

static void
fresh_zone(void)
{
    memset(&queue, 0, sizeof(queue));
    memset(&conf, 0, sizeof(conf));
    queue.shpool = &pool;
    locks = errors = 0;
}

static void
assert_untouched(const ngx_http_cache_purge_queue_t *before)
{
    assert(queue.class_n == before->class_n);
    assert(memcmp(queue.class_names, before->class_names,
                  sizeof(queue.class_names)) == 0);
}

int main(void)
{
    ngx_http_cache_purge_queue_t  before;
    char                          longest[NGX_CACHE_PURGE_CLASS_NAME_MAX + 1];
    char                          other[NGX_CACHE_PURGE_CLASS_NAME_MAX + 1];

    assert(NGX_CACHE_PURGE_CLASSES == 3);

    fresh_zone();
    assert(bind_names(NULL, NULL, NULL) == NGX_OK);
    assert(locks == 0 && queue.class_n == 0 && errors == 0);

    fresh_zone();
    assert(bind_names("bot", "human", NULL) == NGX_OK && !locked);
    assert(conf.traffic_class_slot[0] == 0 && conf.traffic_class_slot[1] == 1);
    assert(queue.class_n == 2);
    assert(strcmp((char *) queue.class_names[0], "bot") == 0);
    assert(strcmp((char *) queue.class_names[1], "human") == 0);

    /* Binding the same configuration again (reload, Windows worker) is a no-op. */
    assert(bind_names("bot", "human", NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 0 && conf.traffic_class_slot[1] == 1);
    assert(queue.class_n == 2 && errors == 0);

    assert(bind_names("human", "bot", NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 1 && conf.traffic_class_slot[1] == 0);
    assert(queue.class_n == 2);

    assert(bind_names("human", NULL, NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 1 && queue.class_n == 2);
    assert(strcmp((char *) queue.class_names[0], "bot") == 0);

    assert(bind_names("bot", "human", NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 0 && conf.traffic_class_slot[1] == 1);
    assert(queue.class_n == 2);

    assert(bind_names("human", "x", NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 1 && conf.traffic_class_slot[1] == 2);
    assert(queue.class_n == 3);

    before = queue;
    errors = 0;
    assert(bind_names("bot", "human", "y") == NGX_ERROR);
    assert(!locked && errors == 1 && last_level == NGX_LOG_EMERG);
    assert_untouched(&before);

    /* A refused attempt consumed nothing: known names still bind, in any order. */
    assert(bind_names("x", "bot", "human") == NGX_OK);
    assert(conf.traffic_class_slot[0] == 2 && conf.traffic_class_slot[1] == 0
           && conf.traffic_class_slot[2] == 1);

    /* Two new names with one free slot: refused as a whole, even though the
     * first of them alone would have fitted. */
    fresh_zone();
    assert(bind_names("a", "b", NULL) == NGX_OK && queue.class_n == 2);
    before = queue;
    errors = 0;
    assert(bind_names("c", "d", NULL) == NGX_ERROR && errors == 1);
    assert_untouched(&before);
    assert(bind_names("a", "b", "c") == NGX_OK);
    assert(conf.traffic_class_slot[2] == 2 && queue.class_n == 3);

    /* Names are compared whole: a prefix is another name. */
    fresh_zone();
    assert(bind_names("bo", NULL, NULL) == NGX_OK);
    assert(bind_names("bot", NULL, NULL) == NGX_OK && conf.traffic_class_slot[0] == 1);
    assert(bind_names("bots", NULL, NULL) == NGX_OK && conf.traffic_class_slot[0] == 2);
    assert(bind_names("bots", "bot", "bo") == NGX_OK);
    assert(conf.traffic_class_slot[0] == 2 && conf.traffic_class_slot[1] == 1
           && conf.traffic_class_slot[2] == 0);

    /* The longest name fits with its terminator; a name differing in the last
     * byte is a different name. */
    fresh_zone();
    memset(longest, 'x', NGX_CACHE_PURGE_CLASS_NAME_MAX);
    longest[NGX_CACHE_PURGE_CLASS_NAME_MAX] = '\0';
    memcpy(other, longest, sizeof(other));
    other[NGX_CACHE_PURGE_CLASS_NAME_MAX - 1] = 'y';
    assert(bind_names(longest, NULL, NULL) == NGX_OK);
    assert(queue.class_names[0][NGX_CACHE_PURGE_CLASS_NAME_MAX] == '\0');
    assert(strlen((char *) queue.class_names[0]) == NGX_CACHE_PURGE_CLASS_NAME_MAX);
    assert(bind_names(other, longest, NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 1 && conf.traffic_class_slot[1] == 0);

    /* Check only (init_shm_zone): slots assigned, zone not written. */
    fresh_zone();
    commit_mode = 0;
    assert(bind_names("bot", "human", NULL) == NGX_OK && !locked);
    assert(conf.traffic_class_slot[0] == 0 && conf.traffic_class_slot[1] == 1);
    assert(queue.class_n == 0);
    assert(queue.class_names[0][0] == '\0' && queue.class_names[1][0] == '\0');

    commit_mode = 1;
    assert(bind_names("bot", "human", NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 0 && conf.traffic_class_slot[1] == 1);
    assert(queue.class_n == 2);
    assert(strcmp((char *) queue.class_names[0], "bot") == 0);
    assert(strcmp((char *) queue.class_names[1], "human") == 0);

    commit_mode = 0;
    before = queue;
    assert(bind_names("human", "bot", NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 1 && conf.traffic_class_slot[1] == 0);
    assert_untouched(&before);

    assert(bind_names("human", "x", NULL) == NGX_OK);
    assert(conf.traffic_class_slot[0] == 1 && conf.traffic_class_slot[1] == 2);
    assert_untouched(&before);
    assert(queue.class_names[2][0] == '\0');

    fresh_zone();
    commit_mode = 1;
    assert(bind_names("a", "b", "c") == NGX_OK && queue.class_n == 3);
    before = queue;
    commit_mode = 0;
    errors = 0;
    assert(bind_names("a", "b", "d") == NGX_ERROR);
    assert(!locked && errors == 1 && last_level == NGX_LOG_EMERG);
    assert_untouched(&before);

    /* A reload that passes the check but fails later is never committed. */
    fresh_zone();
    commit_mode = 1;
    assert(bind_names("a", "b", NULL) == NGX_OK && queue.class_n == 2);
    before = queue;
    commit_mode = 0;
    assert(bind_names("a", "b", "c") == NGX_OK && conf.traffic_class_slot[2] == 2);
    assert_untouched(&before);
    assert(bind_names("a", "b", "d") == NGX_OK && conf.traffic_class_slot[2] == 2);
    commit_mode = 1;
    assert(bind_names("a", "b", "d") == NGX_OK && conf.traffic_class_slot[2] == 2);
    assert(queue.class_n == 3);
    assert(strcmp((char *) queue.class_names[2], "d") == 0);
    /* "c" never reached the zone, so it needs a slot and the zone is now full. */
    errors = 0;
    assert(bind_names("a", "b", "c") == NGX_ERROR && errors == 1);

    fresh_zone();
    commit_mode = 0;
    assert(bind_names(NULL, NULL, NULL) == NGX_OK && locks == 0);
    commit_mode = 1;
    assert(bind_names(NULL, NULL, NULL) == NGX_OK && locks == 0
           && queue.class_n == 0);

    assert(!locked);
    return 0;
}
