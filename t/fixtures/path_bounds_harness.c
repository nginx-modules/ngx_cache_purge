static void
check(char *root, char *input, char *temp, unsigned int expected)
{
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_str_t path, name;
    protection_t protection;
    long page;
    u_char *memory;
    size_t len;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cache_root.data = (u_char *) root;
    ctx.cache_root.len = strlen(root);
    if (temp != NULL) {
        name.data = (u_char *) temp;
        name.len = strlen(temp);
        protection.names = &name;
        protection.nelts = 1;
        ctx.protection = &protection;
    }
    page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    memory = mmap(NULL, (size_t) page * 2, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, (size_t) page, PROT_NONE) == 0);
    len = strlen(input);
    assert(len < (size_t) page);
    path.data = memory + page - len;
    path.len = len;
    memcpy(path.data, input, len); /* Deliberately no trailing zero. */
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path)
           == expected);
    assert(munmap(memory, (size_t) page * 2) == 0);
}

int
main(void)
{
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_str_t path, names[1];
    protection_t protection;
    unsigned int state, count;
    size_t i, len;
    u_char input[256];

    check("/var/cache", "/var/cache/proxy_temp/", "/var/cache/proxy_temp", 1);
    check("/var/cache", "/var/cache/proxy_temp/abc", "/var/cache/proxy_temp", 1);
    check("/var/cache", "/var/cache/main/app_proxy_temp_file", NULL, 0);
    check("/var/cache", "/var/cache/proxy_temp2/file", "/var/cache/proxy_temp", 0);
    check("/var/cache", "/var/cache/app/proxy_temp/file", "/var/cache/proxy_temp", 0);
    check("/var/cache", "/var/cache/proxy_temp/file", NULL, 0);
    check("/proxy_temp/cache", "/proxy_temp/cache/main/entry", NULL, 0);
    check("/var/cache///", "/var/cache/main/entry", NULL, 0);
    check("/", "/proxy_temp/entry", "/proxy_temp", 1);
    check("/var/cache", "/var/cache-other/proxy_temp/entry", NULL, 1);
    check("/var/cache", "", NULL, 1);
#if (NGX_WIN32)
    check("C:\\Cache", "c:/cache\\PROXY_TEMP\\entry", "C:/Cache/proxy_temp", 1);
    check("C:/", "c:\\proxy_temp\\entry", "C:/proxy_temp", 1);
    check("//server/share/cache", "\\\\server\\share\\cache\\spool", "//server/share/cache/spool", 1);
#else
    check("/var/cache", "/var/cache/name\\proxy_temp/entry", "/var/cache/proxy_temp", 0);
#if (NGX_HAVE_CASELESS_FILESYSTEM)
    check("/var/cache", "/var/cache/PROXY_TEMP/entry", "/var/cache/proxy_temp", 1);
#else
    check("/var/cache", "/var/cache/PROXY_TEMP/entry", "/var/cache/proxy_temp", 0);
#endif
#endif

    ngx_current_msec = 120000;
    assert(ngx_http_cache_purge_age((ngx_msec_t) -1001) == 121001);
    assert(ngx_http_cache_purge_age(120001) == 0);
    assert(ngx_http_cache_purge_age(120000) == 0);

    memset(&ctx, 0, sizeof(ctx));
    ctx.cache_root.data = (u_char *) "/var/cache";
    ctx.cache_root.len = sizeof("/var/cache") - 1;
    names[0].data = (u_char *) "/var/cache/custom-spool";
    names[0].len = sizeof("/var/cache/custom-spool") - 1;
    protection.names = names;
    protection.nelts = 1;
    ctx.protection = &protection;
    path.data = (u_char *) "/var/cache/custom-spool/entry";
    path.len = sizeof("/var/cache/custom-spool/entry") - 1;
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path) == 1);
    path.data = (u_char *) "/var/cache/custom-spool-backup/entry";
    path.len = sizeof("/var/cache/custom-spool-backup/entry") - 1;
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path) == 0);
    path.data = NULL;
    path.len = 0;
    assert(ngx_http_cache_purge_is_protected_path(&ctx, &path) == 1);

    path.data = (u_char *) "/cache/0123456789abcdef0123456789abcdef";
    path.len = strlen((char *) path.data);
    assert(ngx_http_cache_purge_is_cache_file(&path) == 1);
    path.data = (u_char *) "/cache/0000000123";
    path.len = strlen((char *) path.data);
    assert(ngx_http_cache_purge_is_cache_file(&path) == 0);
    path.data = (u_char *) "/cache/0123456789abcdef0123456789abcdef.0000000123";
    path.len = strlen((char *) path.data);
    assert(ngx_http_cache_purge_is_cache_file(&path) == 0);

    memset(&ctx, 0, sizeof(ctx));
    ctx.cache_root.data = (u_char *) "/";
    ctx.cache_root.len = 1;
    state = 12345;
    for (count = 0; count < 100000; count++) {
        state = state * 1664525U + 1013904223U;
        len = state % sizeof(input);
        for (i = 0; i < len; i++) {
            state = state * 1664525U + 1013904223U;
            input[i] = (u_char) (state >> 24);
        }
        path.data = input;
        path.len = len;
        (void) ngx_http_cache_purge_is_protected_path(&ctx, &path);
        (void) ngx_http_cache_purge_is_cache_file(&path);
    }
    puts("path bounds and delimiter checks passed");
    return 0;
}
