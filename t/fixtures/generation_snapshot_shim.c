ngx_pid_t ngx_pid = 123;
uint32_t ngx_crc32_table256[256];
uint32_t *ngx_crc32_table_short;

typedef ngx_int_t (*ngx_http_cache_purge_rule_visit_pt)(void *ctx,
    const u_char *key, size_t len, uint64_t generation);

typedef struct {
    u_char      *p;
    uint64_t     size;
    ngx_uint_t   count;
    u_char       kind;
} ngx_http_cache_purge_rule_write_t;

typedef struct {
    ngx_http_cache_purge_rule_t  *node;
    size_t                        depth;
} ngx_http_cache_purge_rule_frame_t;

#define ngx_alloc(size, log) malloc(size)

void
ngx_log_error_core(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{
    (void) level;
    (void) log;
    (void) err;
    (void) fmt;
}


static ngx_int_t
ngx_http_cache_purge_state_path(ngx_http_cache_purge_namespace_t *ns,
    const char *suffix, ngx_pid_t pid, u_char *buf, size_t size)
{
    int  n;

    if (pid) {
        n = snprintf((char *) buf, size, "%s/%*s.%ld.%s",
                     ns->state_path->name.data, (int) ns->name.len,
                     ns->name.data, (long) pid, suffix);
    } else {
        n = snprintf((char *) buf, size, "%s/%*s.%s",
                     ns->state_path->name.data, (int) ns->name.len,
                     ns->name.data, suffix);
    }

    return n < 0 || (size_t) n >= size ? NGX_ERROR : NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_sync_state_dir(ngx_http_cache_purge_namespace_t *ns)
{
    (void) ns;
    return NGX_OK;
}
