int main(int argc, char **argv)
{
    ngx_http_cache_purge_generation_t state;
    ngx_http_cache_purge_namespace_t ns;
    ngx_slab_pool_t slab;
    ngx_shm_zone_t zone;
    ngx_str_t a = ngx_string("host:/a"), b = ngx_string("host:/b");
    ngx_str_t prefix = ngx_string("HOST:/*"), all = ngx_string("*");
    ngx_str_t empty = ngx_null_string, one = ngx_string("x");
    ngx_str_t one_prefix = ngx_string("X*"), longest;
    ngx_str_t key, logical;
    u_char buffer[256];
    u_char long_key[NGX_CACHE_PURGE_LOGICAL_KEY_MAX + 1];
    uint64_t generation;
    unsigned i, before;

    memset(&state, 0, sizeof(state));
    memset(&ns, 0, sizeof(ns));
    memset(&zone, 0, sizeof(zone));
    memset(&slab, 0, sizeof(slab));
    ns.zone = &zone;
    ns.shared = &state;
    ngx_str_set(&ns.name, "images");
    memcpy(state.epoch, "0000000001", 10);
    state.serial = state.baseline = 1;
    zone.shm.addr = (u_char *) &slab;
    assert(ngx_http_cache_purge_generation_lookup(&state, &a) == 1);
    key.data = buffer;
    key.len = snprintf((char *) buffer, sizeof(buffer),
                       "p4:images:0000000001:%016llx:host:/a", 1ULL);
    assert(ngx_http_cache_purge_generation_current(&ns, &key, &logical) == NGX_OK);
    assert(logical.len == a.len && memcmp(logical.data, a.data, a.len) == 0);
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &a, 0, NULL) == NGX_OK);
    assert(ngx_http_cache_purge_generation_lookup(&state, &a) == 2);
    assert(ngx_http_cache_purge_generation_lookup(&state, &b) == 1);
    assert(ngx_http_cache_purge_generation_stale(&ns, &key) == NGX_OK);
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &a, 0, &key) == NGX_DECLINED);
    assert(state.serial == 2); /* old REFRESH cannot invalidate a new generation */
    for (i = 0; i < 10000; i++) {
        assert(ngx_http_cache_purge_generation_invalidate(&ns, &a, 0, NULL) == NGX_OK);
    }
    assert(live == a.len); /* repeated invalidations allocate no new rules */
    assert(ngx_http_cache_purge_generation_lookup(&state, &a) == 10002);
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &prefix, 0, NULL) == NGX_OK);
    assert(ngx_http_cache_purge_generation_lookup(&state, &b) == state.serial);
    generation = state.serial;
    key.len = snprintf((char *) buffer, sizeof(buffer),
                       "p4:images:0000000001:%016llx:host:/a",
                       (unsigned long long) generation);
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &a, 0, &key) == NGX_OK);
    assert(ngx_http_cache_purge_generation_lookup(&state, &a) == generation + 1);
    assert(ngx_http_cache_purge_generation_lookup(&state, &b) == generation);
    busy = 1;
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &all, 0, NULL) == NGX_AGAIN);
    assert(state.serial == generation + 1);
    busy = 0;
    fail_alloc = allocations + 1;
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &b, 0, NULL)
           == NGX_CACHE_PURGE_NAMESPACE_FULL);
    assert(state.full == 1); /* explicit storage failure, not silent success */
    assert(ngx_http_cache_purge_generation_lookup(&state, &a) == state.serial);
    assert(state.exact == NULL && state.prefix == NULL); /* pressure broadens safely */
    fail_alloc = 0;
    before = live;
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &all, 0, NULL) == NGX_OK);
    assert(before - live <= 64 && live == 0);

    /* The trie loop must visit a single-byte selector, but never an empty one. */
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &empty, 0, NULL) == NGX_OK);
    assert(state.baseline == state.serial && live == 0);
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &one, 0, NULL) == NGX_OK);
    assert(ngx_http_cache_purge_generation_lookup(&state, &one) == state.serial);
    assert(live == 1 && state.exact->child == NULL);
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &one_prefix, 0, NULL) == NGX_OK);
    assert(ngx_http_cache_purge_generation_lookup(&state, &one) == state.serial);
    assert(live == 2 && state.prefix->child == NULL);

    memset(long_key, 'x', sizeof(long_key));
    longest.data = long_key;
    longest.len = NGX_CACHE_PURGE_LOGICAL_KEY_MAX;
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &longest, 0, NULL) == NGX_OK);
    assert(ngx_http_cache_purge_generation_lookup(&state, &longest) == state.serial);
    assert(live == longest.len + 1); /* the exact rule reuses its first node */
    generation = state.serial;
    longest.len++;
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &longest, 0, NULL) == NGX_ERROR);
    assert(state.serial == generation && !locked);
    longest.len--;
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &longest, 1, NULL) == NGX_OK);
    assert(state.exact == NULL && state.prefix == NULL);
    while (state.garbage != NULL) {
        before = live;
        assert(ngx_http_cache_purge_generation_invalidate(&ns, &empty, 0, NULL) == NGX_OK);
        assert(before - live <= 64);
    }
    assert(live == 0 && !locked);
    state.serial = (uint64_t) -1;
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &a, 0, NULL) == NGX_ERROR);
    assert(state.serial == (uint64_t) -1 && !locked);
    buffer[3] = 'x';
    assert(ngx_http_cache_purge_generation_stale(&ns, &key) == NGX_DECLINED);

    /* Once an early snapshot starts, a late mutation must not escape it. */
    {
        ngx_pool_t    pool;
        ngx_log_t     log;
        ngx_path_t    path;
        ngx_str_t     one, two;
        ngx_http_cache_purge_rule_t  *node;
        uint64_t      serial;

        assert(argc == 2);
        memset(&pool, 0, sizeof(pool));
        memset(&log, 0, sizeof(log));
        memset(&path, 0, sizeof(path));
        pool.log = &log;
        path.name.data = (u_char *) argv[1];
        path.name.len = strlen(argv[1]);
        ns.pool = &pool;
        ns.state_path = &path;
        ns.name.data = (u_char *) "images";
        ns.name.len = 6;
        one.data = (u_char *) "a";
        one.len = 1;
        two.data = (u_char *) "b";
        two.len = 1;

        state.serial = state.baseline = 1;
        state.full = 0;
        state.exact = state.prefix = NULL;
        state.allocated = state.tail = state.garbage = NULL;

        assert(ngx_http_cache_purge_generation_invalidate(&ns, &one,
                                                          0, NULL) == NGX_OK);
        serial = state.serial;
        assert(ngx_http_cache_purge_generation_persist_locked(&ns, 1)
               == NGX_OK);
        assert(ngx_http_cache_purge_generation_invalidate(&ns, &two,
                                                          0, NULL) == NGX_ABORT);
        assert(state.serial == serial);

        /*
         * A link planted at the predictable temporary name must neither be
         * followed nor truncated: its target keeps its bytes, the link is
         * replaced and the snapshot still succeeds.
         */
        {
            char         link_path[512], victim[512], bytes[8];
            struct stat  sb;
            FILE        *f;
            size_t       n;

            snprintf(victim, sizeof(victim), "%s/victim", argv[1]);
            snprintf(link_path, sizeof(link_path), "%s/images.%ld.tmp",
                     argv[1], (long) ngx_pid);

            f = fopen(victim, "w");
            assert(f != NULL);
            assert(fputs("keep", f) >= 0);
            assert(fclose(f) == 0);

            (void) unlink(link_path);
            assert(symlink(victim, link_path) == 0);

            assert(ngx_http_cache_purge_generation_persist_locked(&ns, 0)
                   == NGX_OK);

            f = fopen(victim, "r");
            assert(f != NULL);
            n = fread(bytes, 1, sizeof(bytes), f);
            assert(fclose(f) == 0);
            assert(n == 4 && memcmp(bytes, "keep", 4) == 0);
            assert(lstat(link_path, &sb) == -1); /* renamed to .state */
        }

        while (state.allocated != NULL) {
            node = state.allocated;
            state.allocated = node->allocated;
            ngx_shmtx_lock(&slab.mutex);
            ngx_slab_free_locked(&slab, node);
            ngx_shmtx_unlock(&slab.mutex);
        }
    }

    assert(live == 0 && !locked);
    puts("generation, conditional admission, memory pressure and rollover checked");
    return 0;
}
