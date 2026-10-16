int
main(void)
{
    ngx_http_file_cache_t cache;
    ngx_http_file_cache_sh_t shared;
    ngx_slab_pool_t pool;
    ngx_http_file_cache_node_t node;
    ngx_rbtree_node_t sentinel;
    ngx_str_t path = ngx_string("/cache/000102030405060708090a0b0c0d0e0f");
    u_char key[NGX_HTTP_CACHE_KEY_LEN];
    ngx_uint_t i;

    memset(&cache, 0, sizeof(cache));
    memset(&shared, 0, sizeof(shared));
    memset(&pool, 0, sizeof(pool));
    memset(&node, 0, sizeof(node));
    memset(&sentinel, 0, sizeof(sentinel));
    for (i = 0; i < NGX_HTTP_CACHE_KEY_LEN; i++) {
        key[i] = (u_char) i;
    }
    /* Match the native cache-key representation. */
    memcpy(&node.node.key, key, sizeof(ngx_rbtree_key_t));
    memcpy(node.key, key + sizeof(ngx_rbtree_key_t), sizeof(node.key));
    node.node.left = &sentinel;
    node.node.right = &sentinel;
    node.exists = 1;
    node.fs_size = 3;
    shared.size = 9;
    shared.rbtree.root = &node.node;
    shared.rbtree.sentinel = &sentinel;
    cache.sh = &shared;
    cache.shpool = &pool;
    ngx_http_cache_purge_invalidate_node(&cache, &path);
    assert(!locked && !node.exists && node.fs_size == 0 && shared.size == 6);
    ngx_http_cache_purge_invalidate_node(&cache, &path);
    assert(!locked && shared.size == 6);
    puts("native cache-key representation and idempotent accounting passed");
    return 0;
}
