int main(void)
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
    assert(ngx_http_cache_purge_generation_invalidate(&ns, &b, 0, NULL) == NGX_OK);
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
    puts("generation, conditional admission, memory pressure and rollover checked");
    return 0;
}
