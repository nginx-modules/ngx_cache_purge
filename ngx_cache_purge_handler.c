/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * Copyright (C) 2016-2026 Denis Denisov
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

#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
static ngx_int_t
ngx_http_cache_purge_defer(ngx_http_request_t *r);
#endif
#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
static void
ngx_http_cache_purge_request_handler(ngx_event_t *ev);
#endif
#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
static void
ngx_http_cache_purge_request_cleanup(void *data);
#endif
#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
static ngx_int_t
ngx_http_cache_purge_dispatch(ngx_http_request_t *r);
#endif
ngx_int_t
ngx_http_cache_purge_access(ngx_array_t *access, ngx_array_t *access6,
    struct sockaddr *s);
#if (nginx_version >= 1007009)
ngx_int_t
ngx_http_cache_purge_cache_get(ngx_http_request_t *r, ngx_http_upstream_t *u,
    ngx_http_file_cache_t **cache);
#endif
ngx_int_t
ngx_http_cache_purge_init(ngx_http_request_t *r, ngx_http_file_cache_t *cache,
    ngx_http_complex_value_t *cache_key);
void
ngx_http_cache_purge_handler(ngx_http_request_t *r);
ngx_int_t
ngx_http_file_cache_purge(ngx_http_request_t *r);
static ngx_int_t
ngx_http_cache_purge_walk_reply(ngx_http_request_t *r,
    ngx_http_cache_purge_walk_ctx_t *ctx, ngx_int_t rc);
ngx_int_t
ngx_http_cache_purge_all(ngx_http_request_t *r, ngx_http_file_cache_t *cache);
ngx_int_t
ngx_http_cache_purge_partial(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache);
ngx_int_t
ngx_http_cache_purge_is_partial(ngx_http_request_t *r);

# if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
/* Wait for admission while retaining core disconnect and stream handling. */
static ngx_int_t
ngx_http_cache_purge_defer(ngx_http_request_t *r)
{
    ngx_http_cache_purge_request_t  *pr;
    ngx_http_cleanup_t              *cln;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_http_cache_purge_path_t      *path;
    ngx_str_t                       *name;

    cln = ngx_http_cleanup_add(r, sizeof(*pr));
    if (cln == NULL) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }
    pr = cln->data;
    ngx_memzero(pr, sizeof(*pr));
    cln->handler = ngx_http_cache_purge_request_cleanup;
    pr->cleanup = cln;
    pr->request = r;
    pr->event.handler = ngx_http_cache_purge_request_handler;
    pr->event.data = pr;
    pr->event.log = r->connection->log;
    /* Keep core disconnect handling, including HTTP/2 and HTTP/3 streams. */
    if (!r->discard_body) {
        r->read_event_handler = ngx_http_test_reading;
        if (r->connection->read->ready) {
            ngx_post_event(r->connection->read, &ngx_posted_events);

        } else if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
            cln->handler = NULL;
            return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
        }
    }
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    name = &r->cache->file_cache->path->name;
    for (path = cmcf->queue->paths; path != NULL; path = path->next) {
        if (path->name.len == ngx_http_cache_purge_path_length(name)
            && ngx_http_cache_purge_path_equal(path->name.data, name->data,
                                               path->name.len))
        {
            pr->path = path;
            ngx_http_cache_purge_stat_wait(path, 1);
            ngx_http_cache_purge_stat_add(path, NGX_PURGE_OVERFLOW, 1);
            break;
        }
    }
    r->main->count++;
    ngx_add_timer(&pr->event, 10);
    return NGX_DONE;
}


static void
ngx_http_cache_purge_request_handler(ngx_event_t *ev)
{
    ngx_http_cache_purge_request_t   *pr = ev->data;
    ngx_http_request_t               *r = pr->request;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_str_t                       *key;
    ngx_int_t                        rc;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    key = r->cache->keys.elts;
    rc = ngx_http_cache_purge_enqueue(r, r->cache->file_cache, &key[0],
                                      cplcf->conf->purge_all, NULL);
    if (rc == NGX_AGAIN) {
        ngx_add_timer(ev, 10);
        return;
    }
    pr->cleanup->handler = NULL;
    ngx_http_cache_purge_stat_wait(pr->path, -1);
    if (rc == NGX_OK) {
        rc = ngx_http_cache_purge_reply(r, cplcf->namespace != NULL
                 ? NGX_PURGE_RESULT_INVALIDATED : NGX_PURGE_RESULT_ACCEPTED);
    } else {
        rc = rc == NGX_ABORT ? NGX_HTTP_CLOSE
             : ngx_http_cache_purge_reply(r, rc == NGX_DECLINED
                 ? NGX_PURGE_RESULT_INVALID_SELECTOR : NGX_PURGE_RESULT_INTERNAL_ERROR);
    }
    ngx_http_finalize_request(r, rc);
}


static void
ngx_http_cache_purge_request_cleanup(void *data)
{
    ngx_http_cache_purge_request_t  *pr = data;

    if (pr->event.timer_set) {
        ngx_del_timer(&pr->event);
    }
    ngx_http_cache_purge_stat_wait(pr->path, -1);
}


static ngx_int_t
ngx_http_cache_purge_dispatch(ngx_http_request_t *r)
{
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_http_cache_purge_loc_conf_t *conf;
    ngx_http_file_cache_t *cache;
    ngx_str_t *key;
    ngx_int_t rc;
    size_t len;

    cache = r->cache->file_cache;
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    conf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    key = r->cache->keys.elts;
    len = key[0].len;
    if ((conf->namespace != NULL && len > NGX_CACHE_PURGE_LOGICAL_KEY_MAX)
        || (conf->namespace == NULL && !conf->conf->purge_all
            && ((ngx_http_cache_purge_is_partial(r)
                 && len > NGX_CACHE_PURGE_KEY_MAX_LEN)
                || (!ngx_http_cache_purge_is_partial(r) && cmcf->vary_aware
                    && len >= NGX_CACHE_PURGE_KEY_MAX_LEN))))
    {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_cache_purge: selector of %uz bytes exceeds the "
                      "supported limit, not purging", len);
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INVALID_SELECTOR);
    }
# if (NGX_HTTP_PROXY)
    if (conf->refresh) {
        return ngx_http_cache_refresh_start(r);
    }
# endif
    if (conf->namespace != NULL
        || (cmcf->background_purge
            && (conf->conf->purge_all || ngx_http_cache_purge_is_partial(r))))
    {
        rc = ngx_http_cache_purge_enqueue(r, cache, key, conf->conf->purge_all, NULL);
        if (rc == NGX_OK) {
            return ngx_http_cache_purge_reply(r, conf->namespace != NULL
                       ? NGX_PURGE_RESULT_INVALIDATED : NGX_PURGE_RESULT_ACCEPTED);
        }
        if (rc == NGX_AGAIN) {
            return ngx_http_cache_purge_defer(r);
        }
        return rc == NGX_ABORT ? NGX_HTTP_CLOSE
               : ngx_http_cache_purge_reply(r, rc == NGX_DECLINED
                   ? NGX_PURGE_RESULT_INVALID_SELECTOR : NGX_PURGE_RESULT_INTERNAL_ERROR);
    }
    if (conf->conf->purge_all) {
        return ngx_http_cache_purge_all(r, cache);
    }
    if (ngx_http_cache_purge_is_partial(r)) {
        return ngx_http_cache_purge_partial(r, cache);
    }
    r->main->count++;
    ngx_http_cache_purge_handler(r);
    return NGX_DONE;
}
# endif

ngx_int_t
ngx_http_cache_purge_access_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_loc_conf_t  *cplcf;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /* A content handler needs a resolved per-protocol configuration. */
    if (cplcf->conf == NULL) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }

    if (r->method_name.len != cplcf->conf->method.len
        || ngx_strncmp(r->method_name.data, cplcf->conf->method.data,
                       r->method_name.len) != 0)
    {
        /*
         * Not a purge request.  Forward to the original content handler
         * if one exists (e.g. proxy_pass), otherwise return 405 with Allow.
         * original_handler is NULL when proxy_cache is used without
         * proxy_pass (cache-only / purge-only location).
         */
        if (cplcf->original_handler != NULL) {
            return cplcf->original_handler(r);
        }
        return ngx_http_cache_purge_method_response(r, &cplcf->conf->method);
    }

    if ((cplcf->conf->access || cplcf->conf->access6)
        && ngx_http_cache_purge_access(cplcf->conf->access,
                                       cplcf->conf->access6,
                                       r->connection->sockaddr) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_cache_purge: access denied for %V",
                      &r->connection->addr_text);
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: client address is not in "
                       "the \"from\" list");
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_ACCESS_DENIED);
    }

    if (cplcf->handler == NULL) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }

    return cplcf->handler(r);
}


ngx_int_t
ngx_http_cache_purge_access(ngx_array_t *access, ngx_array_t *access6,
    struct sockaddr *s)
{
    in_addr_t        inaddr;
    ngx_in_cidr_t  *a;
    ngx_uint_t      i;
# if (NGX_HAVE_INET6)
    struct in6_addr *inaddr6;
    ngx_in6_cidr_t  *a6;
    u_char          *p;
    ngx_uint_t       n;
# else
    (void) access6;
# endif

    switch (s->sa_family) {
    case AF_INET:
        if (access == NULL) {
            return NGX_DECLINED;
        }

        inaddr = ((struct sockaddr_in *) s)->sin_addr.s_addr;

# if (NGX_HAVE_INET6)
ipv4:
# endif
        a = access->elts;
        for (i = 0; i < access->nelts; i++) {
            if ((inaddr & a[i].mask) == a[i].addr) {
                return NGX_OK;
            }
        }
        return NGX_DECLINED;

# if (NGX_HAVE_INET6)
    case AF_INET6:
        inaddr6 = &((struct sockaddr_in6 *) s)->sin6_addr;
        p       = inaddr6->s6_addr;

        if (access && IN6_IS_ADDR_V4MAPPED(inaddr6)) {
            inaddr  = (in_addr_t) p[12] << 24;
            inaddr += (in_addr_t) p[13] << 16;
            inaddr += (in_addr_t) p[14] << 8;
            inaddr += p[15];
            inaddr  = htonl(inaddr);
            goto ipv4;
        }

        if (access6 == NULL) {
            return NGX_DECLINED;
        }

        a6 = access6->elts;
        for (i = 0; i < access6->nelts; i++) {
            for (n = 0; n < 16; n++) {
                if ((p[n] & a6[i].mask.s6_addr[n]) != a6[i].addr.s6_addr[n]) {
                    goto next;
                }
            }
            return NGX_OK;
next:
            continue;
        }
        return NGX_DECLINED;
# endif
    }

    return NGX_DECLINED;
}

# if (nginx_version >= 1007009)
ngx_int_t
ngx_http_cache_purge_cache_get(ngx_http_request_t *r, ngx_http_upstream_t *u,
    ngx_http_file_cache_t **cache)
{
    ngx_str_t               *name;
    ngx_str_t                val;
    ngx_uint_t               i;
    ngx_http_file_cache_t  **caches;

    if (u->conf->cache_zone) {
        *cache = u->conf->cache_zone->data;
        return NGX_OK;
    }

    if (u->conf->cache_value == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_cache_purge: no cache configured for this location");
        return NGX_HTTP_NOT_FOUND;
    }

    if (ngx_http_complex_value(r, u->conf->cache_value, &val) != NGX_OK) {
        return NGX_ERROR;
    }

    if (val.len == 0
        || (val.len == 3 && ngx_strncmp(val.data, "off", 3) == 0))
    {
        return NGX_DECLINED;
    }

    caches = u->caches->elts;

    for (i = 0; i < u->caches->nelts; i++) {
        name = &caches[i]->shm_zone->shm.name;
        if (name->len == val.len
            && ngx_strncmp(name->data, val.data, val.len) == 0)
        {
            *cache = caches[i];
            return NGX_OK;
        }
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "ngx_cache_purge: cache zone \"%V\" not found", &val);

    return NGX_ERROR;
}
# endif


#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
static ngx_int_t
ngx_http_cache_purge_init_response(ngx_http_request_t *r, ngx_int_t rc)
{
    if (rc >= NGX_HTTP_BAD_REQUEST) {
        /* Core rejected the body before the module could execute. */
        return rc;
    }
    return ngx_http_cache_purge_reply(r,
             rc == NGX_BUSY ? NGX_PURGE_RESULT_INVALID_SELECTOR
             : rc == NGX_DECLINED ? NGX_PURGE_RESULT_KEY_MISMATCH
             : NGX_PURGE_RESULT_INTERNAL_ERROR);
}

#endif

/* -- request init ------------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_init(ngx_http_request_t *r, ngx_http_file_cache_t *cache,
    ngx_http_complex_value_t *cache_key)
{
    ngx_http_cache_t  *c;
    ngx_str_t         *key;
    ngx_int_t          rc;
    ngx_http_cache_purge_loc_conf_t *conf;
    ngx_str_t          logical = ngx_null_string;
    ngx_str_t          encoded = ngx_null_string;

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    c = ngx_pcalloc(r->pool, sizeof(ngx_http_cache_t));
    if (c == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_array_init(&c->keys, r->pool, 1, sizeof(ngx_str_t));
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    key = ngx_array_push(&c->keys);
    if (key == NULL) {
        return NGX_ERROR;
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (conf->namespace != NULL) {
        rc = ngx_http_cache_purge_generation_key(r, conf->namespace,
                                    &conf->logical_key, &logical, &encoded);
        if (rc != NGX_OK) {
            return rc == NGX_DECLINED ? NGX_BUSY : NGX_ERROR;
        }
    }
    rc = ngx_http_complex_value(r, cache_key, key);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }
    if (conf->namespace != NULL) {
        if (key->len != encoded.len
            || ngx_memcmp(key->data, encoded.data, encoded.len))
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "ngx_cache_purge: native cache key for namespace "
                          "\"%V\" must be $cache_purge_key",
                          &conf->namespace->name);
            return NGX_DECLINED;
        }
        *key = logical;
    }

    r->cache      = c;
    c->body_start = ngx_pagesize;
    c->file_cache = cache;
    c->file.log   = r->connection->log;

    ngx_http_file_cache_create_key(r);

    return NGX_OK;
}


/* -- purge dispatch ----------------------------------------------------- */

void
ngx_http_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_int_t rc, walk_rc;

# if (NGX_HAVE_FILE_AIO)
    if (r->aio) {
        return;
    }
# endif
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    rc = ngx_http_file_cache_purge(r);
    switch (rc) {
    case NGX_OK:
    case NGX_DECLINED:
        r->write_event_handler = ngx_http_request_empty_handler;
        ngx_memzero(&ctx, sizeof(ctx));
        walk_rc = NGX_OK;
        if (cmcf->vary_aware) {
            walk_rc = ngx_http_cache_purge_delete_variants(r,
                                             r->cache->file_cache, &ctx);
        }
        if (rc == NGX_OK && ctx.files_deleted != (ngx_uint_t) -1) {
            ctx.files_deleted++;
        }
        ngx_http_finalize_request(r, ngx_http_cache_purge_walk_reply(r, &ctx, walk_rc));
        return;
# if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        r->write_event_handler = ngx_http_cache_purge_handler;
        return;
# endif
    case NGX_BUSY:
        ngx_http_finalize_request(r, ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_DELETE_FAILED));
        return;
    default:
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_cache_purge: cache open returned %i", rc);
        ngx_http_finalize_request(r, ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR));
    }
}


/* -- file cache purge --------------------------------------------------- */

ngx_int_t
ngx_http_file_cache_purge(ngx_http_request_t *r)
{
    ngx_http_file_cache_t  *cache;
    ngx_http_cache_t       *c;
    ngx_int_t               rc;

    switch (ngx_http_file_cache_open(r)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
# if (nginx_version >= 8001) \
     || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
# endif
        break;

    case NGX_DECLINED:
        return NGX_DECLINED;

# if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        return NGX_AGAIN;
# endif

    default:
        return NGX_ERROR;
    }

    c     = r->cache;
    cache = c->file_cache;

    ngx_shmtx_lock(&cache->shpool->mutex);

    if (!c->node->exists) {
        ngx_shmtx_unlock(&cache->shpool->mutex);
        return NGX_DECLINED;
    }

# if (nginx_version >= 1000001)
    cache->sh->size -= c->node->fs_size;
    c->node->fs_size  = 0;
# else
    cache->sh->size -= (c->node->length + cache->bsize - 1) / cache->bsize;
    c->node->length   = 0;
# endif

    c->node->exists = 0;
# if (nginx_version >= 8001) \
     || ((nginx_version < 8000) && (nginx_version >= 7060))
    c->node->updating = 0;
# endif

    ngx_shmtx_unlock(&cache->shpool->mutex);

    rc = ngx_http_cache_purge_unlink(r->connection->log, c->file.name.data, NULL);
    if (rc == NGX_OK)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: deleted \"%V\"", &c->file.name);
    }

    return rc == NGX_ERROR ? NGX_BUSY : rc;
}


static ngx_int_t
ngx_http_cache_purge_walk_reply(ngx_http_request_t *r,
    ngx_http_cache_purge_walk_ctx_t *ctx, ngx_int_t rc)
{
    ngx_http_cache_purge_result_t result;

    ngx_memzero(&result, sizeof(result));
    result.counted = 1;
    result.deleted = ctx->files_deleted;
    result.delete_errors = ctx->delete_errors;
    result.scan_errors = ctx->scan_errors;
    result.code = rc == NGX_ABORT ? NGX_PURGE_RESULT_INTERNAL_ERROR
                  : ctx->delete_errors ? NGX_PURGE_RESULT_DELETE_FAILED
                  : (ctx->scan_errors || rc != NGX_OK) ? NGX_PURGE_RESULT_SCAN_FAILED
                  : ctx->files_deleted ? NGX_PURGE_RESULT_PURGED : NGX_PURGE_RESULT_NO_MATCH;
    return ngx_http_cache_purge_send_result(r, &result);
}


/* -- bulk walk helpers -------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_all(ngx_http_request_t *r, ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_int_t rc;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    ngx_http_cache_purge_walk_init(&ctx, &cache->path->name,
                                   cmcf->protection, cmcf->paths, ngx_cycle->log);
    rc = ngx_http_cache_purge_walk(&ctx);
    return ngx_http_cache_purge_walk_reply(r, &ctx, rc);
}


ngx_int_t
ngx_http_cache_purge_partial(ngx_http_request_t *r, ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_str_t *key;
    ngx_int_t rc;

    key = r->cache->keys.elts;
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    ngx_http_cache_purge_walk_init(&ctx, &cache->path->name,
                                   cmcf->protection, cmcf->paths, ngx_cycle->log);
    ctx.key_partial = key[0].data;
    ctx.key_len = key[0].len - 1;
    rc = ngx_http_cache_purge_walk(&ctx);
    return ngx_http_cache_purge_walk_reply(r, &ctx, rc);
}


ngx_int_t
ngx_http_cache_purge_is_partial(ngx_http_request_t *r)
{
    ngx_http_cache_t  *c   = r->cache;
    ngx_str_t         *key = c->keys.elts;

    return c->keys.nelts > 0
        && key[0].len > 0
        && key[0].data[key[0].len - 1] == '*';
}


#if (NGX_HTTP_FASTCGI)
ngx_int_t
ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_file_cache_t             *cache;
    ngx_http_fastcgi_loc_conf_t       *flcf;
#  if (nginx_version >= 1007009)
    ngx_http_fastcgi_main_conf_t  *fmcf;
#  endif

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }

    flcf = ngx_http_get_module_loc_conf(r, ngx_http_fastcgi_module);
    r->upstream->conf = &flcf->upstream;

#  if (nginx_version >= 1007009)
    fmcf = ngx_http_get_module_main_conf(r, ngx_http_fastcgi_module);
    r->upstream->caches = &fmcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_CACHE_UNAVAILABLE);
    }
#  else
    cache = flcf->upstream.cache->data;
#  endif

    rc = ngx_http_cache_purge_init(r, cache, &flcf->cache_key);
    if (rc != NGX_OK) {
        return ngx_http_cache_purge_init_response(r, rc);
    }

    return ngx_http_cache_purge_dispatch(r);
}
#endif

#if (NGX_HTTP_PROXY)
ngx_int_t
ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_file_cache_t             *cache;
    ngx_http_proxy_loc_conf_t         *plcf;
    ngx_http_cache_purge_loc_conf_t   *cplcf;
#  if (nginx_version >= 1007009)
    ngx_http_proxy_main_conf_t  *pmcf;
    ngx_uint_t                   i;
    ngx_str_t                   *name;
    ngx_http_file_cache_t      **caches;
    ngx_str_t                    cv_val;
#  endif

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /* Resolve separate-location state independently of proxy_pass. */
    if (cplcf->proxy.enable == 0
        && (cplcf->proxy_separate_zone || cplcf->proxy_separate_value))
    {
        if (cplcf->proxy_separate_zone) {
            cache = cplcf->proxy_separate_zone->data;
        } else {
            /* dynamic zone name -- evaluate and walk the proxy caches list */
            if (ngx_http_complex_value(r, cplcf->proxy_separate_value,
                                       &cv_val) != NGX_OK)
            {
                return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
            }

#  if (nginx_version >= 1007009)
            pmcf   = ngx_http_get_module_main_conf(r, ngx_http_proxy_module);
            caches = pmcf->caches.elts;
            cache  = NULL;

            for (i = 0; i < pmcf->caches.nelts; i++) {
                name = &caches[i]->shm_zone->shm.name;
                if (name->len == cv_val.len
                    && ngx_strncmp(name->data, cv_val.data, cv_val.len) == 0)
                {
                    cache = caches[i];
                    break;
                }
            }

            if (cache == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "ngx_cache_purge: cache zone \"%V\" not found",
                              &cv_val);
                return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_CACHE_UNAVAILABLE);
            }
#  else
            cache = cplcf->proxy_separate_zone->data;
#  endif
        }

        rc = ngx_http_cache_purge_init(r, cache,
                                      &cplcf->proxy_separate_key);
        if (rc != NGX_OK) {
            return ngx_http_cache_purge_init_response(r, rc);
        }

    } else {
        /* Inline syntax (proxy_cache_purge METHOD from ...): use plcf->upstream
         * as before. */
        if (ngx_http_upstream_create(r) != NGX_OK) {
            return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
        }

        plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);
        r->upstream->conf = &plcf->upstream;

#  if (nginx_version >= 1007009)
        pmcf = ngx_http_get_module_main_conf(r, ngx_http_proxy_module);
        r->upstream->caches = &pmcf->caches;

        rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
        if (rc != NGX_OK) {
            return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_CACHE_UNAVAILABLE);
        }
#  else
        cache = plcf->upstream.cache->data;
#  endif

        rc = ngx_http_cache_purge_init(r, cache, &plcf->cache_key);
        if (rc != NGX_OK) {
            return ngx_http_cache_purge_init_response(r, rc);
        }
    }

    return ngx_http_cache_purge_dispatch(r);
}
#endif

#if (NGX_HTTP_SCGI)
ngx_int_t
ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_file_cache_t             *cache;
    ngx_http_scgi_loc_conf_t          *slcf;
#  if (nginx_version >= 1007009)
    ngx_http_scgi_main_conf_t  *smcf;
#  endif

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_scgi_module);
    r->upstream->conf = &slcf->upstream;

#  if (nginx_version >= 1007009)
    smcf = ngx_http_get_module_main_conf(r, ngx_http_scgi_module);
    r->upstream->caches = &smcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_CACHE_UNAVAILABLE);
    }
#  else
    cache = slcf->upstream.cache->data;
#  endif

    rc = ngx_http_cache_purge_init(r, cache, &slcf->cache_key);
    if (rc != NGX_OK) {
        return ngx_http_cache_purge_init_response(r, rc);
    }

    return ngx_http_cache_purge_dispatch(r);
}
#endif

#if (NGX_HTTP_UWSGI)
ngx_int_t
ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_file_cache_t             *cache;
    ngx_http_uwsgi_loc_conf_t         *ulcf;
#  if (nginx_version >= 1007009)
    ngx_http_uwsgi_main_conf_t  *umcf;
#  endif

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }

    ulcf = ngx_http_get_module_loc_conf(r, ngx_http_uwsgi_module);
    r->upstream->conf = &ulcf->upstream;

#  if (nginx_version >= 1007009)
    umcf = ngx_http_get_module_main_conf(r, ngx_http_uwsgi_module);
    r->upstream->caches = &umcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_CACHE_UNAVAILABLE);
    }
#  else
    cache = ulcf->upstream.cache->data;
#  endif

    rc = ngx_http_cache_purge_init(r, cache, &ulcf->cache_key);
    if (rc != NGX_OK) {
        return ngx_http_cache_purge_init_response(r, rc);
    }

    return ngx_http_cache_purge_dispatch(r);
}
#endif


#endif
