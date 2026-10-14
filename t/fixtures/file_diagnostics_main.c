int
main(void)
{
    ngx_log_t log;
    u_char name[] = "/cache/entry";

    log = 0;
    file_result = 0;
    assert(ngx_http_cache_purge_unlink(&log, name, NULL) == NGX_OK);
    assert(logged == 0);

    file_result = NGX_FILE_ERROR;
    errno = ENOENT;
    assert(ngx_http_cache_purge_unlink(&log, name, NULL) == NGX_DECLINED);
    assert(logged == 0);
    ngx_http_cache_purge_open_failed(&log, name, ngx_open_file_n);
    assert(logged == 0);

    errno = EACCES;
    assert(ngx_http_cache_purge_unlink(&log, name, NULL) == NGX_ERROR);
    assert(logged == 1 && logged_level == NGX_LOG_CRIT);
    assert(logged_errno == EACCES);
    assert(strcmp(message, "ngx_cache_purge: unlink() \"/cache/entry\" failed") == 0);

    errno = EMFILE;
    ngx_http_cache_purge_open_failed(&log, name, ngx_open_file_n);
    assert(logged == 2 && logged_level == NGX_LOG_CRIT);
    assert(logged_errno == EMFILE);
    assert(strcmp(message, "ngx_cache_purge: open() \"/cache/entry\" failed") == 0);

    errno = EROFS;
    assert(ngx_http_cache_purge_unlink(&log, name, NULL) == NGX_ERROR);
    assert(logged == 3 && logged_errno == EROFS);

    errno = EMFILE;
    ngx_http_cache_purge_open_failed(&log, name, "openat()");
    assert(logged == 4 && logged_level == NGX_LOG_CRIT);
    assert(logged_errno == EMFILE);
    assert(strcmp(message, "ngx_cache_purge: openat() \"/cache/entry\" failed") == 0);

    puts("file-error logging contracts passed");
    return 0;
}
