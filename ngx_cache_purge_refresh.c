/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * Copyright (C) 2016-2026, Denys Denysov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ngx_config.h>
#include "ngx_cache_purge.h"
#include "ngx_cache_purge_upstream.h"

#if (NGX_HTTP_CACHE)
/* -- bounded conditional REFRESH ---------------------------------------- */

# if (NGX_HTTP_PROXY)

enum {
    NGX_REFRESH_PENDING, NGX_REFRESH_KEPT, NGX_REFRESH_INVALIDATED,
    NGX_REFRESH_TRANSPORT, NGX_REFRESH_STATUS, NGX_REFRESH_VARY,
    NGX_REFRESH_PROFILE, NGX_REFRESH_REPLACED, NGX_REFRESH_ERROR
};

typedef struct {
    u_char                        hash[32];
    u_char                        key[NGX_CACHE_PURGE_ENTRY_KEY_MAX + 1];
    size_t                        key_len;
    ngx_http_file_cache_header_t  header;
    ngx_uint_t                    status, result;
} ngx_http_cache_refresh_entry_t;

typedef struct {
    /* The visitor receives this first member, never a live HTTP request. */
    ngx_http_cache_purge_walk_ctx_t walk;
    ngx_http_cache_purge_cursor_t   cursor;
    ngx_path_t                     path;
    ngx_str_t                      filter;
    u_char                         after[32];
    ngx_http_cache_refresh_entry_t entries[NGX_CACHE_PURGE_REFRESH_BATCH];
    ngx_uint_t                     count;
    ngx_flag_t                     more, complete, failed, timed_out;
    ngx_msec_t                     deadline;
    ngx_atomic_t                   cancelled;
} ngx_http_cache_refresh_scan_t;

typedef struct {
    ngx_pool_t                    *pool;
    ngx_http_request_t            *request;
    ngx_http_cleanup_t            *cleanup;
    ngx_event_t                    event;
    ngx_http_cache_refresh_scan_t *scan;
    ngx_http_cache_purge_queue_t   *queue;
    ngx_http_file_cache_t         *cache;
    ngx_http_cache_purge_loc_conf_t *conf;
    ngx_msec_t                     deadline;
    ngx_uint_t                     position;
    ngx_flag_t                     scanning, active, credit, decision, timed_out;
    u_char                         after[32];
    ngx_str_t                      uri, etag, modified;
    u_char                         date[sizeof("Mon, 28 Sep 1970 06:00:00 GMT")];
# if (NGX_THREADS && !NGX_WIN32)
    ngx_thread_task_t             *task;
    ngx_thread_pool_t             *thread_pool;
# endif
} ngx_http_cache_refresh_t;

/* One bounded batch per worker; additional HTTP requests wait without I/O. */
#if (NGX_HTTP_PROXY)
static ngx_uint_t
ngx_http_cache_refresh_canonical(ngx_http_cache_refresh_scan_t *scan,
    ngx_str_t *path);
#endif
#if (NGX_HTTP_PROXY)
static void
ngx_http_cache_refresh_collect(void *data, ngx_str_t *path);
#endif
#if (NGX_HTTP_PROXY)
static ngx_int_t
ngx_http_cache_refresh_compare(const void *left, const void *right);
#endif
#if (NGX_HTTP_PROXY)
static ngx_int_t
ngx_http_cache_refresh_scan(ngx_http_cache_refresh_scan_t *scan);
#endif
#if (NGX_HTTP_PROXY)
static ngx_int_t
ngx_http_cache_refresh_credit(ngx_http_cache_refresh_t *job, ngx_flag_t take);
#endif
#if (NGX_HTTP_PROXY)
static void
ngx_http_cache_refresh_destroy(ngx_http_cache_refresh_t *job);
#endif
#if (NGX_HTTP_PROXY)
static void
ngx_http_cache_refresh_cleanup(void *data);
#endif
#if (NGX_HTTP_PROXY && NGX_THREADS && !NGX_WIN32)
static void
ngx_http_cache_refresh_thread(void *data, ngx_log_t *log);
#endif
#if (NGX_HTTP_PROXY && NGX_THREADS && !NGX_WIN32)
static void
ngx_http_cache_refresh_thread_done(ngx_event_t *ev);
#endif
#if (NGX_HTTP_PROXY)
static ngx_int_t
ngx_http_cache_refresh_reply(ngx_http_cache_refresh_t *job);
#endif
#if (NGX_HTTP_PROXY)
static ngx_int_t
ngx_http_cache_refresh_probe(ngx_http_cache_refresh_t *job,
    ngx_http_cache_refresh_entry_t *entry);
#endif
#if (NGX_HTTP_PROXY)
static ngx_int_t
ngx_http_cache_refresh_done(ngx_http_request_t *sr, void *data, ngx_int_t rc);
#endif
#if (NGX_HTTP_PROXY)
static void
ngx_http_cache_refresh_step(ngx_event_t *ev);
#endif

static ngx_http_cache_refresh_t *ngx_cache_refresh_current;
static void ngx_http_cache_refresh_step(ngx_event_t *ev);
static ngx_int_t ngx_http_cache_refresh_done(ngx_http_request_t *sr,
    void *data, ngx_int_t rc);


char *
ngx_http_cache_refresh_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cache_purge_loc_conf_t *location = conf;
    ngx_http_core_loc_conf_t        *core;
    char                           *rc;

    rc = ngx_http_proxy_cache_purge_conf(cf, cmd, conf);
    if (rc != NGX_CONF_OK) {
        return rc;
    }
    location->refresh = 1;
    core = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    core->handler = ngx_http_cache_refresh_handler;
    return NGX_CONF_OK;
}


char *
ngx_http_cache_refresh_profile(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cache_purge_loc_conf_t *location = conf;
    ngx_str_t                      *value = cf->args->elts;

    (void) cmd;
    if (location->refresh_location.data != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }
    if (value[1].len == 0 || value[1].data[0] != '/'
        || ngx_strchr(value[1].data, '?') || ngx_strchr(value[1].data, '$'))
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "requires a literal internal location URI "
                           "without arguments or variables; got \"%V\"",
                           &cmd->name, &value[1]);
        return NGX_CONF_ERROR;
    }
    location->refresh_location = value[1];
    location->refresh_prefix = value[2];
    return NGX_CONF_OK;
}


ngx_int_t
ngx_http_cache_refresh_handler(ngx_http_request_t *r)
{
    ngx_str_t allow = ngx_string("REFRESH");

    if (r != r->main || r->method_name.len != 7
        || ngx_memcmp(r->method_name.data, "REFRESH", 7))
    {
        return ngx_http_cache_purge_method_response(r, &allow);
    }
    return ngx_http_proxy_cache_purge_handler(r);
}


/* Admit only the canonical native hash path: at most one record per cursor. */
static ngx_uint_t
ngx_http_cache_refresh_canonical(ngx_http_cache_refresh_scan_t *scan,
    ngx_str_t *path)
{
    size_t i, level, offset, tail;
    u_char *hash;

    if (path->len != scan->path.name.len + 1 + scan->path.len + 32) {
        return 0;
    }
    hash = path->data + path->len - 32;
    offset = scan->path.name.len + 1;
    tail = 32;
    for (i = 0; i < NGX_MAX_PATH_LEVEL; i++) {
        level = scan->path.level[i];
        if (level == 0) {
            break;
        }
        tail -= level;
        if (ngx_memcmp(path->data + offset, hash + tail, level)
            || path->data[offset + level] != '/')
        {
            return 0;
        }
        offset += level + 1;
    }
    return 1;
}


static void
ngx_http_cache_refresh_collect(void *data, ngx_str_t *path)
{
    ngx_http_cache_refresh_scan_t  *scan = data;
    ngx_http_cache_refresh_entry_t *entry;
    ngx_http_cache_purge_walk_ctx_t *walk = &scan->walk;
    ngx_str_t key, logical;
    ngx_uint_t i, largest;
    ssize_t rc;
    size_t len;
    u_char *hash;

    if (!ngx_http_cache_purge_is_cache_file(path)
        || ngx_http_cache_purge_is_protected_path(walk, path)
        || !ngx_http_cache_refresh_canonical(scan, path))
    {
        return;
    }
    hash = path->data + path->len - 32;
    if (ngx_memcmp(hash, scan->after, 32) <= 0) {
        return;
    }
    rc = ngx_http_cache_purge_read_key(walk, path, 0);
    if (rc != 0) {
        if (rc == NGX_ERROR) {
            walk->scan_errors++;
        }
        return;
    }
    key.data = walk->key_buffer;
    key.len = walk->entry_key_len;
    if (ngx_http_cache_purge_generation_current(walk->namespace, &key, &logical)
        != NGX_OK)
    {
        return;
    }
    len = scan->filter.len;
    if (len != 0 && scan->filter.data[len - 1] == '*') {
        len--;
        if (logical.len < len
            || ngx_strncasecmp(logical.data, scan->filter.data, len))
        {
            return;
        }
    } else if (logical.len != len
               || ngx_memcmp(logical.data, scan->filter.data, len))
    {
        return;
    }

    largest = 0;
    for (i = 0; i < scan->count; i++) {
        if (ngx_memcmp(hash, scan->entries[i].hash, 32) == 0) {
            return;
        }
        if (ngx_memcmp(scan->entries[i].hash, scan->entries[largest].hash, 32) > 0) {
            largest = i;
        }
    }
    if (scan->count == NGX_CACHE_PURGE_REFRESH_BATCH) {
        scan->more = 1;
        if (ngx_memcmp(hash, scan->entries[largest].hash, 32) >= 0) {
            return;
        }
        entry = &scan->entries[largest];
    } else {
        entry = &scan->entries[scan->count++];
    }
    ngx_memcpy(entry->hash, hash, 32);
    ngx_memcpy(entry->key, key.data, key.len);
    entry->key_len = key.len;
    entry->header = walk->header;
}


static ngx_int_t
ngx_http_cache_refresh_compare(const void *left, const void *right)
{
    const ngx_http_cache_refresh_entry_t *a = left, *b = right;
    return ngx_memcmp(a->hash, b->hash, 32);
}


static ngx_int_t
ngx_http_cache_refresh_scan(ngx_http_cache_refresh_scan_t *scan)
{
    ngx_int_t rc;

    ngx_memory_barrier();
    if (scan->cancelled
        || (ngx_msec_int_t) (ngx_current_msec - scan->deadline) >= 0)
    {
        scan->timed_out = 1;
        return NGX_DONE;
    }
    rc = ngx_http_cache_purge_walk_step(&scan->cursor, NGX_CACHE_PURGE_WALK_ENTRIES);
    if (rc != NGX_AGAIN) {
        scan->complete = rc == NGX_OK;
        scan->failed = rc != NGX_OK;
        if (scan->complete) {
            ngx_sort(scan->entries, scan->count, sizeof(scan->entries[0]),
                     ngx_http_cache_refresh_compare);
        }
    }
    return rc;
}


static ngx_int_t
ngx_http_cache_refresh_credit(ngx_http_cache_refresh_t *job, ngx_flag_t take)
{
    ngx_int_t rc;

    if (!take && !job->credit) {
        return NGX_OK;
    }
    rc = ngx_http_cache_purge_probe_credit(job->queue, take);
    if (rc == NGX_OK) {
        job->credit = take;
    }
    return rc;
}


static void
ngx_http_cache_refresh_destroy(ngx_http_cache_refresh_t *job)
{
    if (job->event.timer_set) {
        ngx_del_timer(&job->event);
    }
    if (job->scan != NULL) {
        while (job->scan->cursor.top != NULL) {
            ngx_http_cache_purge_walk_close(&job->scan->cursor);
        }
        if (job->scan->cursor.path.data != NULL) {
            ngx_free(job->scan->cursor.path.data);
        }
    }
    if (ngx_cache_refresh_current == job) {
        ngx_cache_refresh_current = NULL;
    }
    ngx_destroy_pool(job->pool);
}


static void
ngx_http_cache_refresh_cleanup(void *data)
{
    ngx_http_cache_refresh_t *job = data;

    job->request = NULL;
    (void) ngx_http_cache_refresh_credit(job, 0);
    if (job->scan != NULL) {
        job->scan->cancelled = 1;
        ngx_memory_barrier();
    }
# if (NGX_THREADS && !NGX_WIN32)
    if (job->task != NULL && job->task->event.active) {
        return;
    }
# endif
    ngx_http_cache_refresh_destroy(job);
}


# if (NGX_THREADS && !NGX_WIN32)
static void
ngx_http_cache_refresh_thread(void *data, ngx_log_t *log)
{
    ngx_http_cache_refresh_scan_t *scan = data;
    (void) log;
    while (ngx_http_cache_refresh_scan(scan) == NGX_AGAIN) { /* bounded slices */ }
}


static void
ngx_http_cache_refresh_thread_done(ngx_event_t *ev)
{
    ngx_http_cache_refresh_t *job = ev->data;

    job->scanning = 0;
    if (job->request == NULL) {
        ngx_http_cache_refresh_destroy(job);
        return;
    }
    if (job->event.timer_set) {
        ngx_del_timer(&job->event);
    }
    ngx_http_cache_refresh_step(&job->event);
}
# endif


static ngx_int_t
ngx_http_cache_refresh_reply(ngx_http_cache_refresh_t *job)
{
    ngx_http_request_t *r = job->request;
    ngx_http_cache_refresh_scan_t *scan = job->scan;
    ngx_http_cache_refresh_entry_t *entry;
    ngx_uint_t i, count, complete;
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_http_cache_purge_result_code_t code;
    ngx_int_t rc;
    static const char *result[] = {
        "undispatched", "kept", "invalidated", "transport_error",
        "upstream_status", "skipped_vary", "skipped_profile", "raced_replaced",
        "invalidation_error"
    };

    if (scan != NULL && scan->failed) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_SCAN_FAILED);
    }
    count = scan != NULL && scan->complete ? scan->count : 0;
    complete = scan != NULL && scan->complete && !scan->more
               && job->position == count;
    code = complete ? NGX_PURGE_RESULT_REFRESH_COMPLETE : NGX_PURGE_RESULT_REFRESH_INCOMPLETE;
    b = ngx_create_temp_buf(r->pool, 1024 + count * 160);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    b->last = ngx_sprintf(b->last,
        "{\"code\":\"%s\",\"scan_complete\":%s,\"complete\":%s,\"timed_out\":%s,"
        "\"scan_error\":%s,\"undispatched\":%ui,\"next_cursor\":\"%*s\",\"results\":[",
        complete ? "refresh_complete" : "refresh_incomplete",
        scan != NULL && scan->complete ? "true" : "false",
        complete ? "true" : "false", job->timed_out ? "true" : "false",
        scan != NULL && scan->failed ? "true" : "false",
        (scan != NULL ? scan->count : 0) - job->position,
        (size_t) 32, job->after);
    for (i = 0; i < count; i++) {
        entry = &scan->entries[i];
        b->last = ngx_sprintf(b->last,
              "%s{\"hash\":\"%*s\",\"upstream_status\":%ui,\"result\":\"%s\"}",
              i ? "," : "", (size_t) 32, entry->hash, entry->status,
              entry->result < sizeof(result) / sizeof(result[0])
                  ? result[entry->result] : "internal_error");
    }
    b->last = ngx_cpymem(b->last, "]}", 2);
    b->last_buf = 1;
    b->last_in_chain = 1;
    if (ngx_http_cache_purge_response_headers(r, code) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_length_n = b->last - b->pos;
    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }
    out.buf = b;
    out.next = NULL;
    return ngx_http_output_filter(r, &out);
}


static ngx_int_t
ngx_http_cache_refresh_probe(ngx_http_cache_refresh_t *job,
    ngx_http_cache_refresh_entry_t *entry)
{
    ngx_http_post_subrequest_t *post;
    ngx_http_request_t *sr, *r = job->request;
    ngx_str_t encoded, logical;
    size_t i;

    encoded.data = entry->key;
    encoded.len = entry->key_len;
    if (ngx_http_cache_purge_generation_current(job->conf->namespace,
                                               &encoded, &logical) != NGX_OK)
    {
        entry->result = NGX_REFRESH_REPLACED;
        return NGX_DECLINED;
    }
    /* This profile describes one representation and cannot reconstruct the
     * request headers of a Vary entry from the native cache header. */
# ifdef NGX_HTTP_CACHE_VARY_LEN
    if (entry->header.vary_len != 0) {
        entry->result = NGX_REFRESH_VARY;
        return NGX_DECLINED;
    }
# endif
    if (logical.len <= job->conf->refresh_prefix.len
        || ngx_memcmp(logical.data, job->conf->refresh_prefix.data,
                      job->conf->refresh_prefix.len))
    {
        entry->result = NGX_REFRESH_PROFILE;
        return NGX_DECLINED;
    }
    job->uri.data = logical.data + job->conf->refresh_prefix.len;
    job->uri.len = logical.len - job->conf->refresh_prefix.len;
    if (job->uri.data[0] != '/') {
        entry->result = NGX_REFRESH_PROFILE;
        return NGX_DECLINED;
    }
    for (i = 0; i < job->uri.len; i++) {
        if (job->uri.data[i] <= ' ' || job->uri.data[i] == '#'
            || job->uri.data[i] == 127)
        {
            entry->result = NGX_REFRESH_PROFILE;
            return NGX_DECLINED;
        }
    }
    job->etag.data = entry->header.etag;
    job->etag.len = entry->header.etag_len;
    for (i = 0; i < job->etag.len; i++) {
        if (job->etag.data[i] < ' ' || job->etag.data[i] == 127) {
            entry->result = NGX_REFRESH_PROFILE;
            return NGX_DECLINED;
        }
    }
    job->modified.data = job->date;
    job->modified.len = 0;
    if (entry->header.last_modified > 0) {
        job->modified.len = ngx_http_time(job->date, entry->header.last_modified)
                            - job->date;
    }
    if (ngx_http_cache_refresh_credit(job, 1) != NGX_OK) {
        return NGX_AGAIN;
    }
    post = ngx_palloc(r->pool, sizeof(*post));
    if (post == NULL) {
        (void) ngx_http_cache_refresh_credit(job, 0);
        return NGX_ERROR;
    }
    post->handler = ngx_http_cache_refresh_done;
    post->data = job;
    if (ngx_http_subrequest(r, &job->conf->refresh_location, NULL, &sr, post,
                            NGX_HTTP_SUBREQUEST_WAITED) != NGX_OK)
    {
        (void) ngx_http_cache_refresh_credit(job, 0);
        return NGX_ERROR;
    }
    sr->method = NGX_HTTP_HEAD;
    ngx_str_set(&sr->method_name, "HEAD");
    sr->header_only = 1;
    sr->request_body = NULL;
    job->active = 1;
    return NGX_OK;
}


/* Every terminal probe path releases its one credit and schedules progress. */
static ngx_int_t
ngx_http_cache_refresh_done(ngx_http_request_t *sr, void *data, ngx_int_t rc)
{
    ngx_http_cache_refresh_t *job = data;
    ngx_http_cache_refresh_entry_t *entry;

    (void) rc;
    if (!job->active) {
        return NGX_OK;
    }
    job->active = 0;
    (void) ngx_http_cache_refresh_credit(job, 0);
    entry = &job->scan->entries[job->position];
    entry->status = sr->upstream != NULL ? sr->upstream->headers_in.status_n : 0;
    if (entry->result == NGX_REFRESH_PROFILE) {
        /* A guard failure must not become a transport error or an upstream
         * decision after an error_page internal redirect. */
    } else if (entry->status == 304) {
        entry->result = NGX_REFRESH_KEPT;
    } else if (entry->status == 200 || entry->status == 404 || entry->status == 410) {
        job->decision = 1;
    } else {
        entry->result = entry->status == 0 ? NGX_REFRESH_TRANSPORT : NGX_REFRESH_STATUS;
    }
    if (!job->event.timer_set) {
        ngx_add_timer(&job->event, 1);
    }
    /* Consume the probe's local error status. It belongs in the result row,
     * not in the main request's error_page/termination path. */
    return NGX_OK;
}


static void
ngx_http_cache_refresh_step(ngx_event_t *ev)
{
    ngx_http_cache_refresh_t *job = ev->data;
    ngx_http_cache_refresh_scan_t *scan;
    ngx_http_cache_refresh_entry_t *entry;
    ngx_http_cache_purge_main_conf_t *main;
    ngx_http_request_t *r = job->request;
    ngx_str_t *filter, encoded, logical;
    ngx_int_t rc;

    if (r == NULL) {
        return;
    }
    if ((ngx_msec_int_t) (ngx_current_msec - job->deadline) >= 0
        || ngx_exiting || ngx_quit || ngx_terminate)
    {
        job->timed_out = 1;
        if (job->scan != NULL) {
            job->scan->cancelled = 1;
            ngx_memory_barrier();
        }
    }
    if (job->scanning || job->active) {
        ngx_add_timer(ev, 10);
        return;
    }
    if (job->timed_out) {
        goto finished;
    }
    if (ngx_cache_refresh_current != NULL && ngx_cache_refresh_current != job) {
        ngx_add_timer(ev, 10);
        return;
    }
    if (job->scan == NULL) {
        scan = ngx_pcalloc(job->pool, sizeof(*scan));
        if (scan == NULL) {
            goto failed;
        }
        job->scan = scan;
        ngx_cache_refresh_current = job;
        main = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
        scan->path = *job->cache->path;
        scan->deadline = job->deadline;
        ngx_memcpy(scan->after, job->after, 32);
        filter = r->cache->keys.elts;
        scan->filter.len = filter[0].len;
        scan->filter.data = ngx_pnalloc(job->pool, filter[0].len);
        if (scan->filter.data == NULL) {
            goto failed;
        }
        ngx_memcpy(scan->filter.data, filter[0].data, filter[0].len);
        ngx_http_cache_purge_walk_init(&scan->walk, &scan->path.name,
                                       main->protection, main->paths, ngx_cycle->log);
        scan->walk.namespace = job->conf->namespace;
        scan->walk.visit = ngx_http_cache_refresh_collect;
        scan->cursor.ctx = &scan->walk;
# if (NGX_THREADS && !NGX_WIN32)
        if (ngx_http_cache_purge_threaded()) {
            job->thread_pool = ngx_thread_pool_get((ngx_cycle_t *) ngx_cycle,
                                                   &job->conf->thread_pool_name);
            job->task = ngx_thread_task_alloc(job->pool, 0);
            if (job->task == NULL) {
                goto failed;
            }
            job->task->ctx = scan;
            job->task->handler = ngx_http_cache_refresh_thread;
            job->task->event.handler = ngx_http_cache_refresh_thread_done;
            job->task->event.data = job;
            job->task->event.log = ngx_cycle->log;
        }
# endif
    }
    scan = job->scan;
    if (!scan->complete && !scan->failed && !scan->timed_out) {
# if (NGX_THREADS && !NGX_WIN32)
        if (job->thread_pool != NULL) {
            if (ngx_thread_task_post(job->thread_pool, job->task) == NGX_OK) {
                job->scanning = 1;
            } else {
                job->task->event.active = 0;
            }
            ngx_add_timer(ev, 10);
            return;
        }
# endif
        if (ngx_http_cache_refresh_scan(scan) == NGX_AGAIN) {
            ngx_add_timer(ev, 1);
            return;
        }
    }
    if (!scan->complete) {
        job->timed_out = scan->timed_out;
        goto finished;
    }
    if (job->position == scan->count) {
        goto finished;
    }
    entry = &scan->entries[job->position];
    if (job->decision) {
        encoded.data = entry->key;
        encoded.len = entry->key_len;
        if (ngx_http_cache_purge_generation_current(job->conf->namespace,
                                                   &encoded, &logical) != NGX_OK)
        {
            rc = NGX_DECLINED;
        } else {
            rc = ngx_http_cache_purge_enqueue(r, job->cache, &logical, 0, &encoded);
        }
        if (rc == NGX_AGAIN) {
            ngx_add_timer(ev, 10);
            return;
        }
        entry->result = rc == NGX_OK ? NGX_REFRESH_INVALIDATED
                        : rc == NGX_DECLINED ? NGX_REFRESH_REPLACED : NGX_REFRESH_ERROR;
        job->decision = 0;
    }
    if (entry->result == NGX_REFRESH_PENDING) {
        rc = ngx_http_cache_refresh_probe(job, entry);
        if (rc == NGX_OK || rc == NGX_AGAIN) {
            ngx_add_timer(ev, 10);
            if (rc == NGX_OK) {
                /* Timer/thread callbacks do not drain HTTP posted requests. */
                ngx_http_run_posted_requests(r->connection);
            }
            return;
        }
        if (rc == NGX_ERROR) {
            entry->result = NGX_REFRESH_ERROR;
        }
    }
    ngx_memcpy(job->after, entry->hash, 32);
    job->position++;
    ngx_add_timer(ev, 1);
    return;

finished:
    rc = ngx_http_cache_refresh_reply(job);
    job->cleanup->handler = NULL;
    ngx_http_cache_refresh_destroy(job);
    ngx_http_finalize_request(r, rc);
    return;

failed:
    job->cleanup->handler = NULL;
    ngx_http_cache_refresh_destroy(job);
    ngx_http_finalize_request(r, ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR));
}


ngx_int_t
ngx_http_cache_refresh_start(ngx_http_request_t *r)
{
    ngx_http_cache_refresh_t *job;
    ngx_http_cache_purge_main_conf_t *main;
    ngx_pool_t *pool;
    ngx_http_cleanup_t *cleanup;
    ngx_list_part_t *part;
    ngx_table_elt_t *header;
    ngx_uint_t i, n, found;

    pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ngx_cycle->log);
    if (pool == NULL) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }
    job = ngx_pcalloc(pool, sizeof(*job));
    if (job == NULL) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }
    job->pool = pool;
    job->request = r;
    job->cache = r->cache->file_cache;
    job->conf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    main = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    job->queue = main->queue;
    job->deadline = ngx_current_msec + job->conf->refresh_timeout;
    ngx_memset(job->after, '0', 32);
    found = 0;
    part = &r->headers_in.headers.part;
    for ( ;; part = part->next) {
        header = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (header[i].key.len == sizeof("X-Cache-Purge-Cursor") - 1
                && ngx_strncasecmp(header[i].key.data,
                    (u_char *) "X-Cache-Purge-Cursor", header[i].key.len) == 0)
            {
                if (found++ || header[i].value.len != 32) {
                    goto invalid;
                }
                for (n = 0; n < 32; n++) {
                    if (ngx_hextoi(header[i].value.data + n, 1) == NGX_ERROR) {
                        goto invalid;
                    }
                    job->after[n] = ngx_tolower(header[i].value.data[n]);
                }
            }
        }
        if (part->next == NULL) {
            break;
        }
    }
    cleanup = ngx_http_cleanup_add(r, 0);
    if (cleanup == NULL) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }
    job->cleanup = cleanup;
    cleanup->handler = ngx_http_cache_refresh_cleanup;
    cleanup->data = job;
    job->event.handler = ngx_http_cache_refresh_step;
    job->event.data = job;
    job->event.log = ngx_cycle->log;
    job->event.cancelable = 0;
    if (!r->discard_body) {
        r->read_event_handler = ngx_http_test_reading;
        if (r->connection->read->ready) {
            ngx_post_event(r->connection->read, &ngx_posted_events);
        } else if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
            cleanup->handler = NULL;
            ngx_destroy_pool(pool);
            return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
        }
    }
    r->write_event_handler = ngx_http_request_empty_handler;
    r->main->count++;
    ngx_add_timer(&job->event, 1);
    return NGX_DONE;

invalid:
    ngx_destroy_pool(pool);
    return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INVALID_CURSOR);
}


/* Pure getters. They never mutate the upstream method or cache policy. */
ngx_int_t
ngx_http_cache_refresh_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_cache_refresh_t *job;
    ngx_str_t value;

    if (r->post_subrequest == NULL
        || r->post_subrequest->handler != ngx_http_cache_refresh_done)
    {
        v->not_found = 1;
        return NGX_OK;
    }
    job = r->post_subrequest->data;
    value = data == 0 ? job->uri : data == 1 ? job->etag : job->modified;
    v->data = value.data;
    v->len = value.len;
    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    return NGX_OK;
}


/* Validate the actual target after location lookup, including redirects. */
ngx_int_t
ngx_http_cache_refresh_guard(ngx_http_request_t *r)
{
    ngx_http_proxy_loc_conf_t *proxy;
    ngx_http_core_loc_conf_t *core;
    ngx_http_cache_refresh_t *job;
    ngx_keyval_t *headers;
    ngx_str_t method;
    ngx_uint_t i, host, etag, modified;
    const char *reason;

    if (r->post_subrequest == NULL
        || r->post_subrequest->handler != ngx_http_cache_refresh_done)
    {
        return NGX_DECLINED;
    }
    job = r->post_subrequest->data;
    core = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    proxy = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);
    if (!core->internal) {
        reason = "location must be internal";
        goto invalid;
    }
    if (r->uri.len != job->conf->refresh_location.len
        || ngx_memcmp(r->uri.data, job->conf->refresh_location.data, r->uri.len))
    {
        reason = "internal redirect changed the profile URI";
        goto invalid;
    }
    if (proxy->upstream.pass_request_headers) {
        reason = "\"proxy_pass_request_headers\" must be off";
        goto invalid;
    }
    if (proxy->upstream.pass_request_body) {
        reason = "\"proxy_pass_request_body\" must be off";
        goto invalid;
    }
    if (proxy->upstream.intercept_errors) {
        reason = "\"proxy_intercept_errors\" must be off";
        goto invalid;
    }
# if (nginx_version >= 1007009)
    if (proxy->upstream.cache)
# else
    if (proxy->upstream.cache != NULL)
# endif
    {
        reason = "\"proxy_cache\" must be off";
        goto invalid;
    }
    ngx_str_set(&method, "HEAD");
# if (nginx_version >= 1011006)
    if (proxy->method != NULL
        && ngx_http_complex_value(r, proxy->method, &method) != NGX_OK)
    {
        reason = "could not evaluate \"proxy_method\"";
        goto invalid;
    }
# else
    if (proxy->method.len != 0) {
        method = proxy->method;
        /* Before 1.11.6 the core appends the request-line space at merge. */
        if (method.len != 0 && method.data[method.len - 1] == ' ') {
            method.len--;
        }
    }
# endif
    if (method.len != 4 || ngx_memcmp(method.data, "HEAD", 4)) {
        reason = "\"proxy_method\" must be HEAD";
        goto invalid;
    }
    host = etag = modified = 0;
    headers = proxy->headers_source != NULL ? proxy->headers_source->elts : NULL;
    for (i = 0; headers != NULL && i < proxy->headers_source->nelts; i++) {
        /* No client-controlled representation headers may leak into HEAD.
         * Only the two dedicated conditional getters may be dynamic. */
        if (ngx_strchr(headers[i].value.data, '$') != NULL
            && !((headers[i].key.len == sizeof("If-None-Match") - 1
                  && ngx_strncasecmp(headers[i].key.data,
                       (u_char *) "If-None-Match", headers[i].key.len) == 0)
                 || (headers[i].key.len == sizeof("If-Modified-Since") - 1
                     && ngx_strncasecmp(headers[i].key.data,
                          (u_char *) "If-Modified-Since", headers[i].key.len) == 0)))
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "ngx_cache_purge: REFRESH profile \"%V\" rejected: "
                          "header \"%V\" must have a literal value",
                          &job->conf->refresh_location, &headers[i].key);
            job->scan->entries[job->position].result = NGX_REFRESH_PROFILE;
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (headers[i].key.len == 4
            && ngx_strncasecmp(headers[i].key.data, (u_char *) "Host", 4) == 0)
        {
            host = headers[i].value.len != 0
                   && ngx_strchr(headers[i].value.data, '$') == NULL;
        }
        if (headers[i].key.len == sizeof("If-None-Match") - 1
            && ngx_strncasecmp(headers[i].key.data, (u_char *) "If-None-Match",
                               headers[i].key.len) == 0)
        {
            etag = headers[i].value.len == sizeof("$cache_purge_refresh_etag") - 1
                   && ngx_memcmp(headers[i].value.data, "$cache_purge_refresh_etag",
                                  headers[i].value.len) == 0;
        }
        if (headers[i].key.len == sizeof("If-Modified-Since") - 1
            && ngx_strncasecmp(headers[i].key.data, (u_char *) "If-Modified-Since",
                               headers[i].key.len) == 0)
        {
            modified = headers[i].value.len
                       == sizeof("$cache_purge_refresh_last_modified") - 1
                       && ngx_memcmp(headers[i].value.data,
                           "$cache_purge_refresh_last_modified", headers[i].value.len) == 0;
        }
    }
    if (!host) {
        reason = "a non-empty literal \"Host\" header is required";
        goto invalid;
    }
    if (!etag) {
        reason = "\"If-None-Match\" must be $cache_purge_refresh_etag";
        goto invalid;
    }
    if (!modified) {
        reason = "\"If-Modified-Since\" must be $cache_purge_refresh_last_modified";
        goto invalid;
    }
    return NGX_DECLINED;

invalid:
    job->scan->entries[job->position].result = NGX_REFRESH_PROFILE;
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "ngx_cache_purge: REFRESH profile \"%V\" rejected at "
                  "\"%V\": %s", &job->conf->refresh_location, &r->uri, reason);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
}
# endif


#endif
