static void entry_path(char *path, size_t capacity, const char *directory,
    const char *entry)
{
    size_t len = strlen(directory);
    size_t entry_len = strlen(entry);

    assert(len + entry_len + 2 <= capacity);
    memcpy(path, directory, len);
    path[len++] = '/';
    memcpy(path + len, entry, entry_len + 1);
}

int main(void)
{
    char root[] = "purge-walk-XXXXXX";
    char path[1024], deep[3][1024], component[96];
    char outside[] = "purge-outside-XXXXXX", alias[1024], target[1024];
    FILE *file;
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_http_cache_purge_cursor_t cursor;
    ngx_int_t rc;
    unsigned i, before, calls, allocations;

    assert(mkdtemp(root) != NULL);
    memset(&ctx, 0, sizeof(ctx));
    ctx.cache_root.data = (u_char *) root;
    ctx.cache_root.len = strlen(root);
    for (i = 0; i < 512; i++) {
        snprintf(path, sizeof(path), "%s/%04u", root, i);
        file = fopen(path, "w");
        assert(file != NULL);
        assert(fclose(file) == 0);
    }

    memset(&cursor, 0, sizeof(cursor));
    cursor.ctx = &ctx;
    calls = 0;
    do {
        before = reads;
        rc = ngx_http_cache_purge_walk_step(&cursor, 64);
        assert(reads - before <= 64);
        calls++;
    } while (rc == NGX_AGAIN);
    assert(rc == NGX_OK && matched == 512 && calls > 1);
    assert(allocs == 2 && live == 1 && handles == 0);
    ngx_free(cursor.path.data);
    assert(live == 0);

    clock_step = 1;
    memset(&cursor, 0, sizeof(cursor));
    cursor.ctx = &ctx;
    before = reads;
    assert(ngx_http_cache_purge_walk_step(&cursor, 64) == NGX_AGAIN);
    assert(reads - before == NGX_CACHE_PURGE_WALK_MSEC);
    assert(ngx_http_cache_purge_walk_step(&cursor, 0) == NGX_OK);
    assert(handles == 0);
    ngx_free(cursor.path.data);
    assert(live == 0);

    fail_directory_read = 1;
    assert(ngx_http_cache_purge_walk(&ctx) == NGX_ERROR);
    assert(ctx.scan_errors == 1 && handles == 0 && live == 0);

    for (i = 1; i <= 2; i++) {
        allocs = 0;
        fail_alloc = i;
        assert(ngx_http_cache_purge_walk(&ctx) == NGX_ABORT);
        assert(handles == 0 && live == 0);
    }
    fail_alloc = 0;
    snprintf(path, sizeof(path), "%s/nested", root);
    assert(mkdir(path, 0700) == 0);
    allocs = 0;
    fail_alloc = 3;
    assert(ngx_http_cache_purge_walk(&ctx) == NGX_ABORT);
    assert(handles == 0 && live == 0);
    fail_alloc = 0;
    assert(rmdir(path) == 0);

    /* Growing the shared path must preserve every ancestor and sibling. */
    memset(component, 'x', sizeof(component) - 1);
    component[sizeof(component) - 1] = 0;
    for (i = 0; i < 3; i++) {
        entry_path(deep[i], sizeof(deep[i]),
                   i ? deep[i - 1] : root, component);
        assert(mkdir(deep[i], 0700) == 0);
        entry_path(path, sizeof(path), deep[i], "entry");
        file = fopen(path, "w");
        assert(file != NULL && fclose(file) == 0);
    }
    clock_step = 0;
    matched = allocs = 0;
    memset(&cursor, 0, sizeof(cursor));
    cursor.ctx = &ctx;
    do {
        before = reads;
        rc = ngx_http_cache_purge_walk_step(&cursor, 1);
        assert(reads - before <= 1);
    } while (rc == NGX_AGAIN);
    assert(rc == NGX_OK && matched == 515 && handles == 0 && live == 1);
    allocations = allocs;
    assert(allocations > 4);
    ngx_free(cursor.path.data);

    /* Fail every allocation, including growth with several open parents. */
    for (i = 1; i <= allocations; i++) {
        allocs = 0;
        fail_alloc = i;
        assert(ngx_http_cache_purge_walk(&ctx) == NGX_ABORT);
        assert(handles == 0 && live == 0);
    }
    fail_alloc = 0;
    for (i = 3; i > 0; i--) {
        entry_path(path, sizeof(path), deep[i - 1], "entry");
        assert(unlink(path) == 0 && rmdir(deep[i - 1]) == 0);
    }

    /* A real file outside the cache, a descendant link and a linked root. */
    assert(mkdtemp(outside) != NULL);
    outside_path = realpath(outside, NULL);
    assert(outside_path != NULL);
    snprintf(target, sizeof(target), "%s/0123456789abcdef0123456789abcdef", outside);
    file = fopen(target, "w");
    assert(file != NULL && fclose(file) == 0);
    snprintf(path, sizeof(path), "%s/linked", root);
    assert(symlink(outside_path, path) == 0);
    matched = 0;
    assert(ngx_http_cache_purge_walk(&ctx) == NGX_OK && matched == 512);
    assert(unlink(path) == 0);
    snprintf(alias, sizeof(alias), "%s-link", root);
    assert(symlink(root, alias) == 0);
    ctx.cache_root.data = (u_char *) alias;
    ctx.cache_root.len = strlen(alias);
    matched = 0;
    assert(ngx_http_cache_purge_walk(&ctx) == NGX_OK && matched == 512);
    assert(unlink(alias) == 0);
    ctx.cache_root.data = (u_char *) root;
    ctx.cache_root.len = strlen(root);

#if (NGX_CACHE_PURGE_HAVE_AT)
    /* Replace the child after classification but before openat. */
    snprintf(swap_path, sizeof(swap_path), "%s/swap", root);
    snprintf(moved_path, sizeof(moved_path), "%s-moved", root);
    assert(mkdir(swap_path, 0700) == 0);
    swap_directory = 1;
    matched = 0;
    assert(ngx_http_cache_purge_walk(&ctx) == NGX_OK && matched == 512);
    assert(swap_directory == 0);
    assert(access(target, F_OK) == 0);
    assert(unlink(swap_path) == 0 && rmdir(moved_path) == 0);
#endif
    assert(unlink(target) == 0 && rmdir(outside) == 0);
    free(outside_path);

    for (i = 0; i < 512; i++) {
        snprintf(path, sizeof(path), "%s/%04u", root, i);
        assert(unlink(path) == 0);
    }
    assert(rmdir(root) == 0);
    assert(ngx_http_cache_purge_walk(&ctx) == NGX_ERROR);
    assert(handles == 0 && live == 0);
    return 0;
}
