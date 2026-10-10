#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_cache_refresh.h"

ngx_int_t ngx_http_cache_purge_add_action_header(
    ngx_http_request_t *r, ngx_uint_t refresh);
void ngx_http_cache_purge_drain_temp_pools(ngx_queue_t *queue);
ngx_int_t ngx_http_cache_purge_invalidate_item(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_pool_t *pool,
    ngx_http_cache_purge_invalidate_item_t *item,
    ngx_http_cache_purge_invalidate_result_e *result);
ngx_int_t ngx_http_cache_purge_is_partial(ngx_http_request_t *r);
static ngx_int_t ngx_http_cache_purge_send_cache_key_error(
    ngx_http_request_t *r);
static ngx_flag_t ngx_http_cache_purge_refresh_infer_key_prefix_len(
    ngx_str_t *key, ngx_str_t *targets, ngx_uint_t ntargets,
    ngx_uint_t *prefix_len);
static ngx_int_t ngx_http_cache_purge_refresh_collect_target_tails(
    ngx_http_request_t *r, ngx_str_t *targets, ngx_uint_t max_targets,
    ngx_uint_t *ntargets, ngx_flag_t partial);

static ngx_str_t ngx_http_cache_purge_refresh_bypass_name =
    ngx_string("cache_purge_refresh_bypass");

static ngx_str_t ngx_http_head_method_name = ngx_string("HEAD");

static ngx_int_t ngx_http_cache_purge_refresh_bypass_variable(
    ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_cache_purge_refresh_record_status(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_uint_t status, off_t bytes);
static size_t ngx_http_cache_purge_refresh_status_counts_text_len(
    ngx_array_t *status_counts);
static u_char *ngx_http_cache_purge_refresh_write_status_counts_text(
    u_char *p, ngx_array_t *status_counts);
static size_t ngx_http_cache_purge_refresh_status_bytes_text_len(
    ngx_array_t *status_counts);
static u_char *ngx_http_cache_purge_refresh_write_status_bytes_text(
    u_char *p, ngx_array_t *status_counts);
static size_t ngx_http_cache_purge_refresh_status_counts_json_len(
    ngx_array_t *status_counts);
static u_char *ngx_http_cache_purge_refresh_write_status_counts_json(
    u_char *p, ngx_array_t *status_counts);
static size_t ngx_http_cache_purge_refresh_status_bytes_json_len(
    ngx_array_t *status_counts);
static u_char *ngx_http_cache_purge_refresh_write_status_bytes_json(
    u_char *p, ngx_array_t *status_counts);
static size_t ngx_http_cache_purge_refresh_status_counts_log_len(
    ngx_array_t *status_counts);
static u_char *ngx_http_cache_purge_refresh_write_status_counts_log(
    u_char *p, ngx_array_t *status_counts);
static size_t ngx_http_cache_purge_refresh_status_bytes_log_len(
    ngx_array_t *status_counts);
static u_char *ngx_http_cache_purge_refresh_write_status_bytes_log(
    u_char *p, ngx_array_t *status_counts);
static ngx_int_t ngx_http_cache_purge_refresh_collect_open_file(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_collect_path(
    ngx_http_cache_purge_refresh_ctx_t *rctx, ngx_str_t *path,
    ngx_uint_t exact_match);
static void ngx_http_cache_purge_refresh_start(ngx_http_request_t *r);
static ngx_int_t ngx_http_cache_purge_refresh_fire_subrequest(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_done(ngx_http_request_t *r,
    void *data, ngx_int_t rc);
static ngx_int_t ngx_http_cache_purge_refresh_enqueue_retired_chunk_pool(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_pool_t *pool);
static void ngx_http_cache_purge_refresh_drain_retired_chunk_pools(
    ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_send_response(
    ngx_http_request_t *r);
static void ngx_http_cache_purge_refresh_timeout_handler(ngx_event_t *ev);
static void ngx_http_cache_purge_refresh_mark_timeout(
    ngx_http_cache_purge_refresh_ctx_t *ctx);
static void ngx_http_cache_purge_refresh_pool_cleanup(void *data);
static void ngx_http_cache_purge_refresh_finalize(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_scan_next_chunk(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_enqueue_dir(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_str_t *path);
static ngx_int_t ngx_http_cache_purge_refresh_load_dir(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx,
    ngx_str_t *path);
static void ngx_http_cache_purge_refresh_do_invalidate(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx,
    ngx_http_cache_purge_refresh_file_t *file, ngx_uint_t status);

static ngx_int_t
ngx_http_cache_purge_send_cache_key_error(ngx_http_request_t *r)
{
    ngx_buf_t    *b;
    ngx_chain_t   out;
    ngx_int_t     rc;
    size_t        len;
    const char   *msg;

    msg = "refresh requires proxy_cache_key to end with uri or request_uri, or evaluate to uri/request_uri itself\n";
    len = ngx_strlen(msg);

    r->headers_out.status = NGX_HTTP_BAD_REQUEST;
    r->headers_out.content_type.len = sizeof("text/plain") - 1;
    r->headers_out.content_type.data = (u_char *) "text/plain";
    r->headers_out.content_length_n = len;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->last = ngx_cpymem(b->pos, msg, len);
    b->last_buf = 1;
    b->last_in_chain = 1;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


static ngx_flag_t
ngx_http_cache_purge_refresh_infer_key_prefix_len(ngx_str_t *key,
        ngx_str_t *targets, ngx_uint_t ntargets, ngx_uint_t *prefix_len)
{
    ngx_uint_t  i;
    size_t      j;
    ngx_str_t   tail;
    u_char      ch;

    for (i = 0; i < ntargets; i++) {
        if (targets[i].len > 0 && key->len >= targets[i].len
            && ngx_strncmp(key->data + key->len - targets[i].len,
                           targets[i].data, targets[i].len) == 0)
        {
            *prefix_len = key->len - targets[i].len;
            return 1;
        }
    }

    if (key->len > 0 && key->data[0] == '/') {
        for (j = 0; j < key->len; j++) {
            ch = key->data[j];

            if (ch < 0x21 || ch == '|' || ch == '?' || ch == '#') {
                return 0;
            }
        }

        *prefix_len = 0;
        return 1;
    }

    for (j = 1; j < key->len; j++) {
        if (key->data[j] != '/') {
            continue;
        }

        tail.len = key->len - j;
        tail.data = key->data + j;

        if (tail.len == 0) {
            continue;
        }

        if (tail.data[0] != '/') {
            continue;
        }

        for (i = 0; i < j; i++) {
            ch = key->data[i];

            if (ch < 0x21 || ch == '|' || ch == '/' || ch == '?' || ch == '#') {
                break;
            }
        }

        if (i != j) {
            continue;
        }

        for (i = 0; i < ntargets; i++) {
            if (targets[i].len == tail.len
                && ngx_strncmp(tail.data, targets[i].data, tail.len) == 0)
            {
                *prefix_len = j;
                return 1;
            }
        }
    }

    return 0;
}


static ngx_int_t
ngx_http_cache_purge_refresh_collect_target_tails(ngx_http_request_t *r,
        ngx_str_t *targets, ngx_uint_t max_targets, ngx_uint_t *ntargets,
        ngx_flag_t partial)
{
    ngx_str_t    tail, capture, capture_source;
    ngx_uint_t   i, ncaptures;
    intptr_t     start, end;
    u_char      *p, *data;

    *ntargets = 0;

    tail = r->unparsed_uri;

    if (partial && tail.len > 0 && tail.data[tail.len - 1] == '*') {
        tail.len--;
    }

    if (tail.len > 0 && *ntargets < max_targets) {
        targets[(*ntargets)++] = tail;
    }

    tail = r->uri;

    if (partial && tail.len > 0 && tail.data[tail.len - 1] == '*') {
        tail.len--;
    }

    if (tail.len > 0 && *ntargets < max_targets
            && (*ntargets == 0 || targets[0].len != tail.len
                || ngx_strncmp(targets[0].data, tail.data, tail.len) != 0))
    {
        targets[(*ntargets)++] = tail;
    }

    if (r->captures == NULL || r->captures_data == NULL || r->ncaptures < 2) {
        return NGX_OK;
    }

    capture_source = r->unparsed_uri;
    if (r->captures_data == r->uri.data) {
        capture_source = r->uri;

    } else if (r->captures_data != r->unparsed_uri.data) {
        return NGX_OK;
    }

    data = capture_source.data;
    ncaptures = r->ncaptures / 2;

    for (i = 0; i < ncaptures && *ntargets < max_targets; i++) {
        start = r->captures[i * 2];
        end = r->captures[i * 2 + 1];

        if (start < 0 || end < start
            || (size_t) end > capture_source.len)
        {
            continue;
        }

        capture.data = &data[start];
        capture.len = (size_t) (end - start);

        if (capture.len == 0 || capture.data[0] != '/') {
            continue;
        }

        if (partial && capture.len > 0
            && capture.data[capture.len - 1] == '*')
        {
            capture.len--;
        }

        if (r->args.len == 0) {
            targets[(*ntargets)++] = capture;
            continue;
        }

        p = ngx_pnalloc(r->pool, capture.len + 1 + r->args.len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(p, capture.data, capture.len);
        p[capture.len] = '?';
        ngx_memcpy(p + capture.len + 1, r->args.data, r->args.len);

        tail.data = p;
        tail.len = capture.len + 1 + r->args.len;
        targets[(*ntargets)++] = tail;
    }

    return NGX_OK;
}

/* Restore HEAD requests for refresh subrequests and expose bypass variable. */
static ngx_int_t
ngx_http_cache_purge_refresh_bypass_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_cache_purge_refresh_ctx_t *ctx;

    if (r->parent != NULL) {
        ctx = ngx_http_get_module_ctx(r->parent, ngx_http_cache_purge_module);
        if (ctx != NULL) {
            v->len = 1;
            v->valid = 1;
            v->no_cacheable = 1;
            v->not_found = 0;
            v->data = (u_char *) "1";

            if (r->upstream != NULL && r->method == NGX_HTTP_HEAD) {
                r->upstream->method.len = 0;
                r->upstream->method.data = NULL;
            }

            return NGX_OK;
        }
    }

    v->not_found = 1;
    return NGX_OK;
}


ngx_int_t
ngx_http_cache_purge_add_variable(ngx_conf_t *cf)
{
    ngx_http_variable_t *var;

    var = ngx_http_add_variable(cf, &ngx_http_cache_purge_refresh_bypass_name,
                                NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_cache_purge_refresh_bypass_variable;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_refresh_collect_open_file(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    switch (ngx_http_file_cache_open(r)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
#if (nginx_version >= 8001) || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
#endif
        return ngx_http_cache_purge_refresh_collect_path(ctx,
                                                         &r->cache->file.name,
                                                         1);

    case NGX_DECLINED:
        return NGX_OK;

#if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
#endif
    default:
        return NGX_ERROR;
    }
}


static ngx_int_t
ngx_http_cache_purge_refresh_collect_path(
    ngx_http_cache_purge_refresh_ctx_t *rctx, ngx_str_t *path,
    ngx_uint_t exact_match)
{
    ngx_http_cache_purge_refresh_file_t *file;
    ngx_http_file_cache_header_t header;
    ngx_file_t f;
    ngx_file_info_t fi;
    ngx_str_t path_copy, uri, args, etag, cache_key;
    ngx_http_cache_purge_invalidate_item_t item;
    u_char *key_buf;
    u_char *path_data;
    u_char *uri_data;
    u_char *args_data;
    u_char *etag_data;
    u_char *cache_key_data;
    ssize_t n;
    size_t key_read_len;
    u_char *p, *q;
    ngx_pool_t *pool;

    ngx_memzero(&path_copy, sizeof(ngx_str_t));
    ngx_memzero(&uri, sizeof(ngx_str_t));
    ngx_memzero(&args, sizeof(ngx_str_t));
    ngx_memzero(&etag, sizeof(ngx_str_t));
    ngx_memzero(&cache_key, sizeof(ngx_str_t));
    ngx_memzero(&item, sizeof(ngx_http_cache_purge_invalidate_item_t));

    pool = rctx->chunk_pool != NULL ? rctx->chunk_pool : rctx->request->pool;

    if (rctx->timeout_enabled && !rctx->timed_out
        && ngx_current_msec >= rctx->deadline)
    {
        ngx_http_cache_purge_refresh_mark_timeout(rctx);
        return NGX_ABORT;
    }

    ngx_memzero(&f, sizeof(ngx_file_t));
    f.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN,
                         NGX_FILE_DEFAULT_ACCESS);
    if (f.fd == NGX_INVALID_FILE) {
        return NGX_OK;
    }
    f.name.data = path->data;
    f.name.len = path->len;
    f.log = ngx_cycle->log;

    if (ngx_fd_info(f.fd, &fi) == NGX_FILE_ERROR) {
        ngx_close_file(f.fd);
        return NGX_OK;
    }

    n = ngx_read_file(&f, (u_char *) &header, sizeof(header), 0);
    if (n < (ssize_t) sizeof(header)) {
        ngx_close_file(f.fd);
        return NGX_OK;
    }

    if (header.header_start <= sizeof(header) + 6) {
        ngx_close_file(f.fd);
        return NGX_OK;
    }

    key_read_len = header.header_start - sizeof(header) - 6;
    if (key_read_len == 0 || key_read_len > 8192) {
        ngx_close_file(f.fd);
        return NGX_OK;
    }

    key_buf = ngx_alloc(key_read_len + 1, ngx_cycle->log);
    if (key_buf == NULL) {
        ngx_close_file(f.fd);
        return NGX_ERROR;
    }

    n = ngx_read_file(&f, key_buf, key_read_len, sizeof(header) + 6);
    ngx_close_file(f.fd);

    if (n < 1) {
        ngx_free(key_buf);
        return NGX_OK;
    }

    key_buf[n] = '\0';
    if (n > 0 && key_buf[n - 1] == LF) {
        key_buf[n - 1] = '\0';
        n--;
    }

    if (exact_match) {
        if ((size_t) n != rctx->key_partial.len
            || ngx_strncmp(key_buf, rctx->key_partial.data,
                           rctx->key_partial.len) != 0)
        {
            ngx_free(key_buf);
            return NGX_OK;
        }

    } else if (!rctx->purge_all && rctx->key_partial.len > 0) {
        if ((size_t) n < rctx->key_partial.len) {
            ngx_free(key_buf);
            return NGX_OK;
        }
        if (ngx_strncmp(key_buf, rctx->key_partial.data,
                        rctx->key_partial.len) != 0)
        {
            ngx_free(key_buf);
            return NGX_OK;
        }
    }

    if ((size_t) n < rctx->key_prefix_len) {
        ngx_free(key_buf);
        return NGX_OK;
    }

    path_data = ngx_pnalloc(pool, path->len + 1);
    if (path_data == NULL) {
        ngx_free(key_buf);
        return NGX_ERROR;
    }
    ngx_memcpy(path_data, path->data, path->len);
    path_data[path->len] = '\0';
    path_copy.len = path->len;
    path_copy.data = path_data;
    item.cache_path = path_copy;

    p = key_buf + rctx->key_prefix_len;
    q = (u_char *) ngx_strchr(p, '?');
    if (q != NULL) {
        uri.len = q - p;
        uri_data = ngx_pnalloc(pool, uri.len + 1);
        if (uri_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(uri_data, p, uri.len);
        uri_data[uri.len] = '\0';
        uri.data = uri_data;

        q++;
        if ((size_t) n < rctx->key_prefix_len + uri.len + 1) {
            ngx_free(key_buf);
            return NGX_OK;
        }
        args.len = n - rctx->key_prefix_len - uri.len - 1;
        args_data = ngx_pnalloc(pool, args.len + 1);
        if (args_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(args_data, q, args.len);
        args_data[args.len] = '\0';
        args.data = args_data;
    } else {
        uri.len = n - rctx->key_prefix_len;
        uri_data = ngx_pnalloc(pool, uri.len + 1);
        if (uri_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(uri_data, p, uri.len);
        uri_data[uri.len] = '\0';
        uri.data = uri_data;
    }

    if (header.etag_len > 0 && header.etag_len < NGX_HTTP_CACHE_ETAG_LEN) {
        etag_data = ngx_pnalloc(pool, header.etag_len + 1);
        if (etag_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(etag_data, header.etag, header.etag_len);
        etag_data[header.etag_len] = '\0';
        etag.len = header.etag_len;
        etag.data = etag_data;
    }

    item.etag_len = etag.len;
    if (etag.len > 0) {
        ngx_memcpy(item.etag, etag.data, etag.len);
    }

    item.uniq = ngx_file_uniq(&fi);
    item.last_modified = header.last_modified;
    item.body_start = header.body_start;
    item.fs_size = ngx_file_size(&fi);

    cache_key_data = ngx_pnalloc(pool, n + 1);
    if (cache_key_data == NULL) {
        ngx_free(key_buf);
        return NGX_ERROR;
    }
    ngx_memcpy(cache_key_data, key_buf, n);
    cache_key_data[n] = '\0';
    cache_key.len = n;
    cache_key.data = cache_key_data;
    item.cache_key = cache_key;

    ngx_free(key_buf);

    file = ngx_array_push(rctx->files);
    if (file == NULL) {
        return NGX_ERROR;
    }

    file->path = path_copy;
    file->uri = uri;
    file->args = args;
    file->etag = etag;
    file->last_modified = header.last_modified;
    file->item = item;

    rctx->queued++;
    rctx->total++;
    rctx->total_bytes += item.fs_size;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "refresh collect: uri=\"%V\" etag=\"%V\" path=\"%V\"",
                   &file->uri, &file->etag, &file->path);

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_refresh_record_status(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_uint_t status, off_t bytes)
{
    ngx_uint_t i;
    ngx_http_cache_purge_refresh_status_count_t *entry;

    if (ctx->status_counts == NULL) {
        return NGX_ERROR;
    }

    entry = ctx->status_counts->elts;
    for (i = 0; i < ctx->status_counts->nelts; i++) {
        if (entry[i].status == status) {
            entry[i].count++;
            entry[i].bytes += bytes;
            return NGX_OK;
        }
    }

    entry = ngx_array_push(ctx->status_counts);
    if (entry == NULL) {
        return NGX_ERROR;
    }

    entry->status = status;
    entry->count = 1;
    entry->bytes = bytes;

    return NGX_OK;
}


static size_t
ngx_http_cache_purge_refresh_status_counts_text_len(ngx_array_t *status_counts)
{
    ngx_uint_t i;
    size_t len;

    if (status_counts == NULL || status_counts->nelts == 0) {
        return 0;
    }

    len = sizeof(" statuses={}") - 1;

    for (i = 0; i < status_counts->nelts; i++) {
        len += 2 * NGX_INT_T_LEN;
        if (i != 0) {
            len++;
        }
    }

    return len;
}


static u_char *
ngx_http_cache_purge_refresh_write_status_counts_text(u_char *p,
    ngx_array_t *status_counts)
{
    ngx_uint_t i;
    ngx_http_cache_purge_refresh_status_count_t *entry;

    if (status_counts == NULL || status_counts->nelts == 0) {
        return p;
    }

    entry = status_counts->elts;
    *p++ = ' ';
    ngx_memcpy(p, "statuses={", sizeof("statuses={") - 1);
    p += sizeof("statuses={") - 1;

    for (i = 0; i < status_counts->nelts; i++) {
        if (i != 0) {
            *p++ = ',';
        }

        p = ngx_sprintf(p, "%ui:%ui", entry[i].status, entry[i].count);
    }

    *p++ = '}';

    return p;
}


static size_t
ngx_http_cache_purge_refresh_status_bytes_text_len(ngx_array_t *status_counts)
{
    ngx_uint_t i;
    size_t len;

    if (status_counts == NULL || status_counts->nelts == 0) {
        return 0;
    }

    len = sizeof(" status_bytes={}") - 1;

    for (i = 0; i < status_counts->nelts; i++) {
        len += NGX_INT_T_LEN + NGX_OFF_T_LEN + 1;
        if (i != 0) {
            len++;
        }
    }

    return len;
}


static u_char *
ngx_http_cache_purge_refresh_write_status_bytes_text(u_char *p,
    ngx_array_t *status_counts)
{
    ngx_uint_t i;
    ngx_http_cache_purge_refresh_status_count_t *entry;

    if (status_counts == NULL || status_counts->nelts == 0) {
        return p;
    }

    entry = status_counts->elts;
    *p++ = ' ';
    ngx_memcpy(p, "status_bytes={", sizeof("status_bytes={") - 1);
    p += sizeof("status_bytes={") - 1;

    for (i = 0; i < status_counts->nelts; i++) {
        if (i != 0) {
            *p++ = ',';
        }

        p = ngx_sprintf(p, "%ui:%O", entry[i].status, entry[i].bytes);
    }

    *p++ = '}';

    return p;
}


static size_t
ngx_http_cache_purge_refresh_status_counts_json_len(ngx_array_t *status_counts)
{
    ngx_uint_t i;
    size_t len;

    if (status_counts == NULL || status_counts->nelts == 0) {
        return sizeof(",\"status_counts\":{}") - 1;
    }

    len = sizeof(",\"status_counts\":{}") - 1;

    for (i = 0; i < status_counts->nelts; i++) {
        len += 2 * NGX_INT_T_LEN + 4;
        if (i != 0) {
            len++;
        }
    }

    return len;
}


static size_t
ngx_http_cache_purge_refresh_status_bytes_json_len(ngx_array_t *status_counts)
{
    ngx_uint_t i;
    size_t len;

    if (status_counts == NULL || status_counts->nelts == 0) {
        return sizeof(",\"status_bytes\":{}") - 1;
    }

    len = sizeof(",\"status_bytes\":{}") - 1;

    for (i = 0; i < status_counts->nelts; i++) {
        len += NGX_INT_T_LEN + NGX_OFF_T_LEN + 4;
        if (i != 0) {
            len++;
        }
    }

    return len;
}


static u_char *
ngx_http_cache_purge_refresh_write_status_bytes_json(u_char *p,
    ngx_array_t *status_counts)
{
    ngx_uint_t i;
    ngx_http_cache_purge_refresh_status_count_t *entry;

    ngx_memcpy(p, ",\"status_bytes\":{", sizeof(",\"status_bytes\":{") - 1);
    p += sizeof(",\"status_bytes\":{") - 1;

    if (status_counts != NULL && status_counts->nelts != 0) {
        entry = status_counts->elts;
        for (i = 0; i < status_counts->nelts; i++) {
            if (i != 0) {
                *p++ = ',';
            }

            p = ngx_sprintf(p, "\"%ui\":%O", entry[i].status,
                            entry[i].bytes);
        }
    }

    *p++ = '}';

    return p;
}


static u_char *
ngx_http_cache_purge_refresh_write_status_counts_json(u_char *p,
    ngx_array_t *status_counts)
{
    ngx_uint_t i;
    ngx_http_cache_purge_refresh_status_count_t *entry;

    ngx_memcpy(p, ",\"status_counts\":{", sizeof(",\"status_counts\":{") - 1);
    p += sizeof(",\"status_counts\":{") - 1;

    if (status_counts != NULL && status_counts->nelts != 0) {
        entry = status_counts->elts;
        for (i = 0; i < status_counts->nelts; i++) {
            if (i != 0) {
                *p++ = ',';
            }

            p = ngx_sprintf(p, "\"%ui\":%ui", entry[i].status,
                            entry[i].count);
        }
    }

    *p++ = '}';

    return p;
}


static size_t
ngx_http_cache_purge_refresh_status_counts_log_len(ngx_array_t *status_counts)
{
    if (status_counts == NULL || status_counts->nelts == 0) {
        return 0;
    }

    return ngx_http_cache_purge_refresh_status_counts_text_len(status_counts);
}


static u_char *
ngx_http_cache_purge_refresh_write_status_counts_log(u_char *p,
    ngx_array_t *status_counts)
{
    return ngx_http_cache_purge_refresh_write_status_counts_text(p,
            status_counts);
}


static size_t
ngx_http_cache_purge_refresh_status_bytes_log_len(ngx_array_t *status_counts)
{
    if (status_counts == NULL || status_counts->nelts == 0) {
        return 0;
    }

    return ngx_http_cache_purge_refresh_status_bytes_text_len(status_counts);
}


static u_char *
ngx_http_cache_purge_refresh_write_status_bytes_log(u_char *p,
    ngx_array_t *status_counts)
{
    return ngx_http_cache_purge_refresh_write_status_bytes_text(p,
            status_counts);
}


static void
ngx_http_cache_purge_refresh_do_invalidate(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx,
    ngx_http_cache_purge_refresh_file_t *file, ngx_uint_t status)
{
    ngx_pool_t *pool;
    ngx_http_cache_purge_invalidate_result_e invalidate_result;

    pool = ngx_create_pool(4096, r->connection->log);
    if (pool == NULL) {
        ctx->errors++;
        return;
    }

    if (ngx_http_cache_purge_invalidate_item(ctx->request, ctx->cache,
            pool, &file->item, &invalidate_result) != NGX_OK)
    {
        ctx->errors++;
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                      "refresh invalidate failed (%ui) for \"%V\"",
                      status, &file->uri);
    } else {
        if (invalidate_result == NGX_HTTP_CACHE_PURGE_INVALIDATE_PURGED) {
            ctx->purged++;
            ctx->purged_bytes += file->item.fs_size;
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "refresh: %ui purged \"%V\"", status,
                           &file->uri);
        } else if (invalidate_result
                   == NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_MISSING
                   || invalidate_result
                   == NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_REPLACED)
        {
            ctx->refreshed++;
            ctx->kept_bytes += file->item.fs_size;
            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "refresh: %ui race-kept (%ui) \"%V\"",
                           status, invalidate_result, &file->uri);
        } else {
            ctx->errors++;
        }
    }

    ngx_destroy_pool(pool);
}


static ngx_int_t
ngx_http_cache_purge_refresh_done(ngx_http_request_t *r, void *data,
    ngx_int_t rc)
{
    ngx_http_cache_purge_refresh_post_data_t *pd;
    ngx_http_cache_purge_refresh_ctx_t *ctx;
    ngx_http_cache_purge_refresh_file_t *file;
    ngx_uint_t status;

    pd = data;
    ctx = pd->ctx;

    if (pd->handled) {
        return NGX_OK;
    }

    pd->handled = 1;

    if (ctx->finalized) {
        return NGX_OK;
    }

    file = pd->file;

    if (!pd->validation_ready) {
        ctx->errors++;
        if (ctx->active > 0) {
            ctx->active--;
        }

        return NGX_OK;
    }

    if (rc != NGX_OK) {
        ctx->errors++;
        if (ctx->active > 0) {
            ctx->active--;
        }

        return NGX_OK;
    }

    status = 0;
    if (r->upstream != NULL) {
        status = r->upstream->headers_in.status_n;
    } else if (r->headers_out.status) {
        status = r->headers_out.status;
    }

    if (status != 0
        && ngx_http_cache_purge_refresh_record_status(ctx, status,
                                                       file->item.fs_size)
        != NGX_OK)
    {
        ctx->errors++;
        if (ctx->active > 0) {
            ctx->active--;
        }

        return NGX_OK;
    }

    if (status == NGX_HTTP_NOT_MODIFIED) {
        ctx->refreshed++;
        ctx->kept_bytes += file->item.fs_size;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "refresh: 304 kept \"%V\"", &file->uri);
    } else if (status == NGX_HTTP_OK) {
        ngx_http_cache_purge_refresh_do_invalidate(r, ctx, file, status);
    } else if (status == NGX_HTTP_NOT_FOUND || status == 410) {
        ngx_http_cache_purge_refresh_do_invalidate(r, ctx, file, status);
    } else if (status == 0) {
        ctx->errors++;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "refresh: no response, error \"%V\"", &file->uri);
    } else {
        ctx->refreshed++;
        ctx->kept_bytes += file->item.fs_size;
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "refresh: %ui kept \"%V\"", status, &file->uri);
    }

    if (ctx->active > 0) {
        ctx->active--;
    }

    if (ctx->timed_out) {
        if (ctx->current < ctx->queued) {
            ctx->errors += ctx->queued - ctx->current;
            ctx->current = ctx->queued;
        }
    }

    ngx_http_post_request(ctx->request, NULL);

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_refresh_fire_subrequest(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_http_cache_purge_loc_conf_t *cplcf;
    ngx_http_cache_purge_refresh_file_t *file;
    ngx_http_cache_purge_refresh_post_data_t *pd;
    ngx_http_request_t *sr;
    ngx_http_post_subrequest_t *ps;
    ngx_int_t rc;
    ngx_table_elt_t *h;
    u_char *time_buf;

    if (ctx->current >= ctx->queued) {
        return NGX_OK;
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    if (cplcf->refresh_timeout != 0 && ngx_current_msec >= ctx->deadline) {
        ngx_http_cache_purge_refresh_mark_timeout(ctx);
        return NGX_ABORT;
    }

    file = (ngx_http_cache_purge_refresh_file_t *) ctx->files->elts
           + ctx->current;

    ps = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
    if (ps == NULL) {
        return NGX_ERROR;
    }

    pd = ngx_palloc(r->pool, sizeof(ngx_http_cache_purge_refresh_post_data_t));
    if (pd == NULL) {
        return NGX_ERROR;
    }
    pd->ctx = ctx;
    pd->file = file;
    pd->validation_ready = 0;
    pd->handled = 0;

    ps->handler = ngx_http_cache_purge_refresh_done;
    ps->data = pd;

    rc = ngx_http_subrequest(r, &file->uri,
                             file->args.len > 0 ? &file->args : NULL,
                             &sr, ps,
                             NGX_HTTP_SUBREQUEST_BACKGROUND);
    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "refresh fire subrequest failed rc=%i uri=\"%V\"",
                      rc, &file->uri);
        ctx->errors++;
        return rc;
    }

    ctx->current++;
    ctx->active++;
    ctx->dispatched++;

    sr->method = NGX_HTTP_HEAD;
    sr->method_name = ngx_http_head_method_name;
    sr->header_only = 1;

    if (ngx_list_init(&sr->headers_in.headers, r->pool, 8,
                      sizeof(ngx_table_elt_t)) != NGX_OK)
    {
        pd->handled = 1;
        ctx->errors++;
        if (ctx->active > 0) { ctx->active--; }
        if (ngx_http_post_request(ctx->request, NULL) != NGX_OK) {
            return NGX_ERROR;
        }
        return NGX_OK;
    }

    if (r->headers_in.host != NULL) {
        h = ngx_list_push(&sr->headers_in.headers);
        if (h == NULL) {
            pd->handled = 1;
            ctx->errors++;
            if (ctx->active > 0) { ctx->active--; }
            if (ngx_http_post_request(ctx->request, NULL) != NGX_OK) {
                return NGX_ERROR;
            }
            return NGX_OK;
        }
        *h = *r->headers_in.host;
        sr->headers_in.host = h;
    }

    sr->headers_in.if_none_match = NULL;
    sr->headers_in.if_modified_since = NULL;

    if (file->etag.len > 0) {
        h = ngx_list_push(&sr->headers_in.headers);
        if (h == NULL) {
            pd->handled = 1;
            ctx->errors++;
            if (ctx->active > 0) { ctx->active--; }
            if (ngx_http_post_request(ctx->request, NULL) != NGX_OK) {
                return NGX_ERROR;
            }
            return NGX_OK;
        }
        h->hash = 1;
        ngx_str_set(&h->key, "If-None-Match");
        h->value = file->etag;
        h->lowcase_key = (u_char *) "if-none-match";
        sr->headers_in.if_none_match = h;
    }

    if (file->last_modified > 0) {
        h = ngx_list_push(&sr->headers_in.headers);
        if (h == NULL) {
            pd->handled = 1;
            ctx->errors++;
            if (ctx->active > 0) { ctx->active--; }
            if (ngx_http_post_request(ctx->request, NULL) != NGX_OK) {
                return NGX_ERROR;
            }
            return NGX_OK;
        }
        time_buf = ngx_pnalloc(r->pool,
                               sizeof("Mon, 28 Sep 1970 06:00:00 GMT"));
        if (time_buf == NULL) {
            pd->handled = 1;
            ctx->errors++;
            if (ctx->active > 0) { ctx->active--; }
            if (ngx_http_post_request(ctx->request, NULL) != NGX_OK) {
                return NGX_ERROR;
            }
            return NGX_OK;
        }
        h->hash = 1;
        ngx_str_set(&h->key, "If-Modified-Since");
        h->value.data = time_buf;
        ngx_http_time(time_buf, file->last_modified);
        h->value.len = sizeof("Mon, 28 Sep 1970 06:00:00 GMT") - 1;
        h->lowcase_key = (u_char *) "if-modified-since";
        sr->headers_in.if_modified_since = h;
    }

    pd->validation_ready = 1;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "refresh: fired subrequest for \"%V\" (%ui/%ui)",
                   &file->uri, ctx->current, ctx->queued);

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_refresh_send_response(ngx_http_request_t *r)
{
    ngx_http_cache_purge_refresh_ctx_t *ctx;
    ngx_http_cache_purge_loc_conf_t *cplcf;
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_int_t rc;
    size_t len;
    u_char *p;

    ctx = ngx_http_get_module_ctx(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    if (cplcf->response_type == NGX_CACHE_PURGE_RESPONSE_TYPE_JSON) {
        len = sizeof("{\"status\":\"refresh\",\"total\":,\"kept\":,\"purged\":,\"errors\":,\"total_bytes\":,\"kept_bytes\":,\"purged_bytes\":}")
              + 4 * NGX_INT_T_LEN + 3 * NGX_OFF_T_LEN
              + ngx_http_cache_purge_refresh_status_counts_json_len(ctx->status_counts)
              + ngx_http_cache_purge_refresh_status_bytes_json_len(ctx->status_counts);

        r->headers_out.content_type.len = sizeof("application/json") - 1;
        r->headers_out.content_type.data = (u_char *) "application/json";
    } else {
        len = sizeof("Refresh: total= kept= purged= errors= total_bytes= kept_bytes= purged_bytes=\n")
              + 4 * NGX_INT_T_LEN + 3 * NGX_OFF_T_LEN
              + ngx_http_cache_purge_refresh_status_counts_text_len(ctx->status_counts)
              + ngx_http_cache_purge_refresh_status_bytes_text_len(ctx->status_counts);

        r->headers_out.content_type.len = sizeof("text/plain") - 1;
        r->headers_out.content_type.data = (u_char *) "text/plain";
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (cplcf->response_type == NGX_CACHE_PURGE_RESPONSE_TYPE_JSON) {
        p = ngx_sprintf(b->pos,
                        "{\"status\":\"refresh\",\"total\":%ui,\"kept\":%ui,"
                        "\"purged\":%ui,\"errors\":%ui,"
                        "\"total_bytes\":%O,\"kept_bytes\":%O,\"purged_bytes\":%O",
                        ctx->total, ctx->refreshed, ctx->purged, ctx->errors,
                        ctx->total_bytes, ctx->kept_bytes, ctx->purged_bytes);
        p = ngx_http_cache_purge_refresh_write_status_counts_json(p,
                ctx->status_counts);
        p = ngx_http_cache_purge_refresh_write_status_bytes_json(p,
                ctx->status_counts);
        *p++ = '}';
    } else {
        p = ngx_sprintf(b->pos,
                        "Refresh: total=%ui kept=%ui purged=%ui errors=%ui total_bytes=%O kept_bytes=%O purged_bytes=%O",
                        ctx->total, ctx->refreshed, ctx->purged, ctx->errors,
                        ctx->total_bytes, ctx->kept_bytes, ctx->purged_bytes);
        p = ngx_http_cache_purge_refresh_write_status_counts_text(p,
                ctx->status_counts);
        p = ngx_http_cache_purge_refresh_write_status_bytes_text(p,
                ctx->status_counts);
        *p++ = '\n';
    }

    b->last = p;
    b->last_buf = 1;
    b->last_in_chain = 1;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = p - b->pos;

    if (ngx_http_cache_purge_add_action_header(r, 1) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


static void
ngx_http_cache_purge_refresh_mark_timeout(ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_http_cache_purge_loc_conf_t *cplcf;
    ngx_uint_t skipped;

    if (ctx->timed_out) {
        return;
    }

    ctx->timed_out = 1;
    cplcf = ngx_http_get_module_loc_conf(ctx->request,
                                         ngx_http_cache_purge_module);

    if (ctx->dispatched < ctx->total) {
        skipped = ctx->total - ctx->dispatched;
        ctx->errors += skipped;
        ctx->current = ctx->queued;
        ctx->dispatched = ctx->total;
    } else {
        skipped = 0;
    }

    ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                  "cache purge refresh timed out after %M ms, skipped %ui pending entries",
                  cplcf->refresh_timeout, skipped);
}


static void
ngx_http_cache_purge_refresh_pool_cleanup(void *data)
{
    ngx_http_cache_purge_refresh_ctx_t *ctx;

    ctx = data;

    if (ctx == NULL) {
        return;
    }

    ctx->finalized = 1;

    if (ctx->timeout_ev.timer_set) {
        ngx_del_timer(&ctx->timeout_ev);
    }

    if (ctx->scan_pool != NULL) {
        ngx_destroy_pool(ctx->scan_pool);
        ctx->scan_pool = NULL;
    }

    ngx_http_cache_purge_drain_temp_pools(&ctx->temp_pools);
    ngx_http_cache_purge_refresh_drain_retired_chunk_pools(ctx);

    if (ctx->chunk_pool != NULL) {
        ngx_destroy_pool(ctx->chunk_pool);
        ctx->chunk_pool = NULL;
    }

    if (ctx->retired_chunk_pool != NULL) {
        ngx_destroy_pool(ctx->retired_chunk_pool);
        ctx->retired_chunk_pool = NULL;
    }
}


static ngx_int_t
ngx_http_cache_purge_refresh_enqueue_retired_chunk_pool(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_pool_t *pool)
{
    ngx_http_cache_purge_refresh_temp_pool_t *entry;

    entry = ngx_palloc(ctx->request->pool,
                       sizeof(ngx_http_cache_purge_refresh_temp_pool_t));
    if (entry == NULL) {
        return NGX_ERROR;
    }

    entry->pool = pool;
    ngx_queue_insert_tail(&ctx->retired_chunk_pools, &entry->queue);

    return NGX_OK;
}


static void
ngx_http_cache_purge_refresh_drain_retired_chunk_pools(
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_queue_t *q;
    ngx_http_cache_purge_refresh_temp_pool_t *entry;

    while (!ngx_queue_empty(&ctx->retired_chunk_pools)) {
        q = ngx_queue_head(&ctx->retired_chunk_pools);
        ngx_queue_remove(q);

        entry = ngx_queue_data(q,
                               ngx_http_cache_purge_refresh_temp_pool_t,
                               queue);

        if (entry->pool != NULL) {
            ngx_destroy_pool(entry->pool);
        }
    }
}


static void
ngx_http_cache_purge_refresh_finalize(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_int_t rc;
    size_t len;
    u_char *msg;
    u_char *p;

    if (ctx->finalized) {
        return;
    }

    ctx->finalized = 1;

    if (ctx->timeout_ev.timer_set) {
        ngx_del_timer(&ctx->timeout_ev);
    }

    len = sizeof("cache refresh summary uri=\"\" total= kept= purged= errors= timed_out= total_bytes= kept_bytes= purged_bytes=")
          + r->uri.len + 5 * NGX_INT_T_LEN + 3 * NGX_OFF_T_LEN
          + ngx_http_cache_purge_refresh_status_counts_log_len(ctx->status_counts)
          + ngx_http_cache_purge_refresh_status_bytes_log_len(ctx->status_counts);

    msg = ngx_pnalloc(r->pool, len + 1);
    if (msg == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "cache refresh summary uri=\"%V\" total=%ui kept=%ui purged=%ui errors=%ui timed_out=%ui total_bytes=%O kept_bytes=%O purged_bytes=%O",
                      &r->uri, ctx->total, ctx->refreshed, ctx->purged,
                      ctx->errors, ctx->timed_out ? 1 : 0,
                      ctx->total_bytes, ctx->kept_bytes, ctx->purged_bytes);
    } else {
        p = ngx_sprintf(msg,
                        "cache refresh summary uri=\"%V\" total=%ui kept=%ui purged=%ui errors=%ui timed_out=%ui total_bytes=%O kept_bytes=%O purged_bytes=%O",
                        &r->uri, ctx->total, ctx->refreshed, ctx->purged,
                        ctx->errors, ctx->timed_out ? 1 : 0,
                        ctx->total_bytes, ctx->kept_bytes, ctx->purged_bytes);
        p = ngx_http_cache_purge_refresh_write_status_counts_log(p,
                ctx->status_counts);
        p = ngx_http_cache_purge_refresh_write_status_bytes_log(p,
                ctx->status_counts);
        *p = '\0';

        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "%s", msg);
    }

    rc = ngx_http_cache_purge_refresh_send_response(r);
    ngx_http_finalize_request(r, rc);
}


static ngx_int_t
ngx_http_cache_purge_refresh_enqueue_dir(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_str_t *path)
{
    ngx_http_cache_purge_refresh_dir_t *dir;
    u_char *p;

    dir = ngx_palloc(ctx->request->pool,
                     sizeof(ngx_http_cache_purge_refresh_dir_t));
    if (dir == NULL) {
        return NGX_ERROR;
    }

    p = ngx_pnalloc(ctx->request->pool, path->len + 1);
    if (p == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(p, path->data, path->len);
    p[path->len] = '\0';

    dir->path.len = path->len;
    dir->path.data = p;

    ngx_queue_insert_tail(&ctx->pending_dirs, &dir->queue);

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_refresh_load_dir(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_str_t *path)
{
    ngx_dir_t dir;
    ngx_http_cache_purge_refresh_scan_entry_t *entry;
    ngx_str_t child;
    u_char *name;
    u_char *p;
    size_t len;
    ngx_int_t rc;

    if (ctx->scan_pool != NULL) {
        ngx_destroy_pool(ctx->scan_pool);
        ctx->scan_pool = NULL;
    }

    ctx->scan_pool = ngx_create_pool(4096, r->connection->log);
    if (ctx->scan_pool == NULL) {
        return NGX_ERROR;
    }

    ctx->scan_entries = ngx_array_create(ctx->scan_pool, 64,
                                         sizeof(ngx_http_cache_purge_refresh_scan_entry_t));
    if (ctx->scan_entries == NULL) {
        return NGX_ERROR;
    }

    ctx->scan_index = 0;

    if (ngx_open_dir(path, &dir) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                      ngx_open_dir_n " \"%V\" failed", path);
        return NGX_ERROR;
    }

    rc = NGX_OK;

    for ( ;; ) {
        ngx_set_errno(0);

        if (ngx_read_dir(&dir) == NGX_ERROR) {
            if (ngx_errno != NGX_ENOMOREFILES) {
                ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                              ngx_read_dir_n " \"%V\" failed", path);
                rc = NGX_ERROR;
            }

            break;
        }

        len = ngx_de_namelen(&dir);
        name = ngx_de_name(&dir);

        if (len == 1 && name[0] == '.') {
            continue;
        }

        if (len == 2 && name[0] == '.' && name[1] == '.') {
            continue;
        }

        if (path->len == ctx->cache->path->name.len
            && ngx_strncmp(path->data, ctx->cache->path->name.data,
                           path->len) == 0
            && ngx_de_is_dir(&dir))
        {
            if (len == sizeof("proxy_temp") - 1
                && ngx_strncmp(name, (u_char *) "proxy_temp", len) == 0)
            {
                continue;
            }
        }

        child.len = path->len + 1 + len;
        child.data = ngx_pnalloc(ctx->scan_pool, child.len + 1);
        if (child.data == NULL) {
            rc = NGX_ERROR;
            break;
        }

        p = ngx_cpymem(child.data, path->data, path->len);
        *p++ = '/';
        ngx_memcpy(p, name, len);
        p[len] = '\0';

        if (!dir.valid_info && ngx_de_info(child.data, &dir) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                          ngx_de_info_n " \"%V\" failed", &child);
            continue;
        }

        if (!ngx_de_is_file(&dir) && !ngx_de_is_dir(&dir)) {
            continue;
        }

        entry = ngx_array_push(ctx->scan_entries);
        if (entry == NULL) {
            rc = NGX_ERROR;
            break;
        }

        entry->path = child;
        entry->is_dir = ngx_de_is_dir(&dir);
    }

    if (ngx_close_dir(&dir) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                      ngx_close_dir_n " \"%V\" failed", path);
        if (rc == NGX_OK) {
            rc = NGX_ERROR;
        }
    }

    return rc;
}


static ngx_int_t
ngx_http_cache_purge_refresh_scan_next_chunk(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_http_cache_purge_refresh_dir_t *dir;
    ngx_http_cache_purge_refresh_scan_entry_t *entry;
    ngx_int_t rc;

    if (ctx->scan_done) {
        return NGX_OK;
    }

    if (ctx->retired_chunk_pool != NULL) {
        if (ngx_http_cache_purge_refresh_enqueue_retired_chunk_pool(
                ctx, ctx->retired_chunk_pool) != NGX_OK)
        {
            ngx_destroy_pool(ctx->retired_chunk_pool);
        }
        ctx->retired_chunk_pool = NULL;
    }

    if (ctx->chunk_pool != NULL) {
        ctx->retired_chunk_pool = ctx->chunk_pool;
        ctx->chunk_pool = NULL;
    }

    ctx->chunk_pool = ngx_create_pool(4096, r->connection->log);
    if (ctx->chunk_pool == NULL) {
        return NGX_ERROR;
    }

    ctx->files = ngx_array_create(ctx->chunk_pool, ctx->chunk_limit,
                                  sizeof(ngx_http_cache_purge_refresh_file_t));
    if (ctx->files == NULL) {
        return NGX_ERROR;
    }

    ctx->current = 0;
    ctx->queued = 0;

    if (ctx->exact) {
        ctx->scan_done = 1;
        return ngx_http_cache_purge_refresh_collect_open_file(r, ctx);
    }

    if (!ctx->scan_initialized) {
        ngx_queue_init(&ctx->pending_dirs);
        if (ngx_http_cache_purge_refresh_enqueue_dir(ctx,
                                                     &ctx->cache->path->name)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ctx->scan_initialized = 1;
    }

    for ( ;; ) {
        if (ctx->timeout_enabled && !ctx->timed_out
            && ngx_current_msec >= ctx->deadline)
        {
            ngx_http_cache_purge_refresh_mark_timeout(ctx);
            ctx->scan_done = 1;
            return NGX_OK;
        }

        if (ctx->queued >= ctx->chunk_limit) {
            return NGX_OK;
        }

        if (ctx->scan_entries != NULL
            && ctx->scan_index < ctx->scan_entries->nelts)
        {
            entry = ((ngx_http_cache_purge_refresh_scan_entry_t *)
                     ctx->scan_entries->elts) + ctx->scan_index++;

            if (entry->is_dir) {
                if (ngx_http_cache_purge_refresh_enqueue_dir(ctx, &entry->path)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }

                continue;
            }

            rc = ngx_http_cache_purge_refresh_collect_path(ctx, &entry->path, 0);
            if (rc != NGX_OK) {
                if (rc == NGX_ABORT && ctx->timed_out) {
                    ctx->scan_done = 1;
                    return NGX_OK;
                }

                return rc;
            }

            continue;
        }

        if (ctx->scan_pool != NULL) {
            ngx_destroy_pool(ctx->scan_pool);
            ctx->scan_pool = NULL;
        }

        ctx->scan_entries = NULL;
        ctx->scan_index = 0;

        if (ngx_queue_empty(&ctx->pending_dirs)) {
            ctx->scan_done = 1;
            return NGX_OK;
        }

        dir = (ngx_http_cache_purge_refresh_dir_t *) ngx_queue_data(
                  ngx_queue_head(&ctx->pending_dirs),
                  ngx_http_cache_purge_refresh_dir_t, queue);
        ngx_queue_remove(&dir->queue);

        rc = ngx_http_cache_purge_refresh_load_dir(r, ctx, &dir->path);
        if (rc != NGX_OK) {
            return rc;
        }
    }
}


static void
ngx_http_cache_purge_refresh_timeout_handler(ngx_event_t *ev)
{
    ngx_http_cache_purge_refresh_ctx_t *ctx;

    ctx = ev->data;

    if (ctx == NULL || ctx->finalized) {
        return;
    }

    ngx_http_cache_purge_refresh_mark_timeout(ctx);

    if (ctx->active == 0) {
        ngx_http_cache_purge_refresh_finalize(ctx->request, ctx);
    }
}


static void
ngx_http_cache_purge_refresh_start(ngx_http_request_t *r)
{
    ngx_http_cache_purge_refresh_ctx_t *ctx;
    ngx_http_cache_purge_loc_conf_t *cplcf;
    ngx_int_t rc;
    ngx_uint_t i, concurrency;

    ctx = ngx_http_get_module_ctx(r, ngx_http_cache_purge_module);

    if (ctx == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (ctx->queued == 0 && ctx->scan_done && ctx->total == 0) {
        ngx_http_cache_purge_refresh_finalize(r, ctx);
        return;
    }

    if (ctx->current >= ctx->queued && ctx->active == 0 && ctx->scan_done) {
        ngx_http_cache_purge_refresh_finalize(r, ctx);
        return;
    }

    if (ctx->timed_out) {
        if (ctx->active == 0) {
            ngx_http_cache_purge_refresh_finalize(r, ctx);
        }
        return;
    }

    if (ctx->active == 0 && ctx->current >= ctx->queued && !ctx->scan_done) {
        ctx->current = 0;
        ctx->queued = 0;
    }

    if (ctx->queued == 0) {
        if (!ctx->scan_done) {
            rc = ngx_http_cache_purge_refresh_scan_next_chunk(r, ctx);
            if (rc != NGX_OK) {
                ctx->errors++;
                ngx_http_cache_purge_refresh_finalize(r, ctx);
                return;
            }
        }

        if (ctx->timed_out) {
            ctx->current = ctx->queued;

            if (ctx->active == 0) {
                ngx_http_cache_purge_refresh_finalize(r, ctx);
            }
            return;
        }

        if (ctx->queued == 0) {
            if (ctx->scan_done) {
                ngx_http_cache_purge_refresh_finalize(r, ctx);
            }
            return;
        }
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    concurrency = cplcf->refresh_concurrency;
    if (concurrency == 0) {
        concurrency = 1;
    }
    if (concurrency > ctx->queued) {
        concurrency = ctx->queued;
    }

    for (i = ctx->active; i < concurrency; i++) {
        rc = ngx_http_cache_purge_refresh_fire_subrequest(r, ctx);
        if (rc == NGX_OK) {
            continue;
        }

        if (rc == NGX_ABORT && ctx->timed_out) {
            break;
        }

        ctx->errors += ctx->queued - ctx->current;
        ngx_http_cache_purge_refresh_finalize(r, ctx);
        return;
    }
}


ngx_int_t
ngx_http_cache_purge_refresh(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_refresh_ctx_t *ctx;
    ngx_http_cache_purge_loc_conf_t *cplcf;
    ngx_pool_cleanup_t *cln;
    ngx_str_t *keys;
    ngx_str_t targets[4];
    ngx_str_t key;
    ngx_uint_t ntargets;

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "cache purge refresh in %s", cache->path->name.data);

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_cache_purge_refresh_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_queue_init(&ctx->temp_pools);
    ngx_queue_init(&ctx->retired_chunk_pools);

    ctx->request = r;
    ctx->cache = cache;
    ctx->status_counts = ngx_array_create(r->pool, 4,
                                          sizeof(ngx_http_cache_purge_refresh_status_count_t));
    if (ctx->status_counts == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    cln->handler = ngx_http_cache_purge_refresh_pool_cleanup;
    cln->data = ctx;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    ctx->purge_all = cplcf->conf->purge_all;
    ctx->exact = !ctx->purge_all && !ngx_http_cache_purge_is_partial(r);
    ctx->timeout_enabled = (cplcf->refresh_timeout != 0);
    ctx->deadline = ngx_current_msec + cplcf->refresh_timeout;
    ctx->chunk_limit = cplcf->refresh_concurrency * 4;

    if (!ctx->exact && r->args.len > 0) {
        return ngx_http_cache_purge_send_cache_key_error(r);
    }

    if (ctx->chunk_limit == 0) {
        ctx->chunk_limit = 4;
    }
    if (ctx->chunk_limit < 32) {
        ctx->chunk_limit = 32;
    }

    ngx_memzero(&ctx->timeout_ev, sizeof(ngx_event_t));
    ctx->timeout_ev.handler = ngx_http_cache_purge_refresh_timeout_handler;
    ctx->timeout_ev.data = ctx;
    ctx->timeout_ev.log = r->connection->log;

    if (ctx->timeout_enabled) {
        ngx_add_timer(&ctx->timeout_ev, cplcf->refresh_timeout);
    }

    keys = r->cache->keys.elts;
    key = keys[0];
    if (!ctx->exact && key.len > 0 && key.data[key.len - 1] == '*') {
        key.len--;
    }

    ctx->key_partial.len = key.len;
    ctx->key_partial.data = key.data;

    if (ngx_http_cache_purge_refresh_collect_target_tails(r, targets,
            sizeof(targets) / sizeof(targets[0]), &ntargets, !ctx->exact)
        != NGX_OK)
    {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (!ngx_http_cache_purge_refresh_infer_key_prefix_len(&key, targets,
            ntargets, &ctx->key_prefix_len))
    {
        return ngx_http_cache_purge_send_cache_key_error(r);
    }

    ngx_http_set_ctx(r, ctx, ngx_http_cache_purge_module);
    r->write_event_handler = ngx_http_cache_purge_refresh_start;

#if (nginx_version >= 8011)
    r->main->count++;
#endif

    ngx_http_cache_purge_refresh_start(r);

    return NGX_DONE;
}
