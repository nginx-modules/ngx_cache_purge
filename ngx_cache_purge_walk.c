/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * Copyright (C) 2016-2026 Denys Denysov
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
#include <ngx_md5.h>

#if (NGX_HTTP_CACHE)

static void
ngx_http_cache_purge_open_failed(ngx_log_t *log, u_char *name,
    const char *failed_op);
static ngx_int_t
ngx_http_cache_purge_walk_name(ngx_http_cache_purge_cursor_t *cursor,
    u_char *name, size_t len);
static ngx_int_t
ngx_http_cache_purge_walk_open(ngx_http_cache_purge_cursor_t *cursor);
static ngx_int_t
ngx_http_cache_purge_skip_protected_dir(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path);
static void
ngx_http_cache_purge_invalidate_node(ngx_http_file_cache_t *cache,
    ngx_str_t *path);
static void
ngx_http_cache_purge_delete_file(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path);

ngx_int_t
ngx_http_cache_purge_unlink(ngx_log_t *log, u_char *name, ngx_dir_t *parent)
{
    ngx_err_t    err;
    const char  *failed_op;

    if (ngx_http_cache_purge_delete_entry(name, parent, &failed_op)
        != NGX_FILE_ERROR)
    {
        return NGX_OK;
    }

    err = ngx_errno;
    if (err == NGX_ENOENT) {
        return NGX_DECLINED;
    }

    ngx_log_error(NGX_LOG_CRIT, log, err,
                  "ngx_cache_purge: %s \"%s\" failed", failed_op, name);

    return NGX_ERROR;
}

/* A missing file is an expected race; report other open errors. */
static void
ngx_http_cache_purge_open_failed(ngx_log_t *log, u_char *name,
    const char *failed_op)
{
    ngx_err_t  err;

    err = ngx_errno;
    if (err != NGX_ENOENT) {
        ngx_log_error(NGX_LOG_CRIT, log, err,
                      "ngx_cache_purge: %s \"%s\" failed", failed_op, name);
    }
}


void
ngx_http_cache_purge_walk_init(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *root, ngx_http_cache_purge_protection_t *protection,
    ngx_array_t *paths, ngx_log_t *log)
{
    ngx_memzero(ctx, sizeof(ngx_http_cache_purge_walk_ctx_t));

    ctx->cache_root = *root;
    ctx->protection = protection;
    ctx->paths = paths;
    ctx->log = log;
}

/* Ancestor prefixes stay in one buffer while the walk descends. */
static ngx_int_t
ngx_http_cache_purge_walk_name(ngx_http_cache_purge_cursor_t *cursor,
    u_char *name, size_t len)
{
    size_t   base, size, capacity;
    u_char  *p;

    base = (cursor->top != NULL) ? cursor->top->len : 0;
    if (len > (size_t) -1 - 2 || base > (size_t) -1 - len - 2) {
        return NGX_ABORT;
    }
    size = base + (cursor->top != NULL) + len;

    if (cursor->allocated <= size) {
        capacity = size + 1;
        if (capacity <= (size_t) -1 / 2) {
            capacity *= 2;
        }
        p = ngx_alloc(capacity, cursor->ctx->log);
        if (p == NULL) {
            return NGX_ABORT;
        }
        if (cursor->path.data != NULL) {
            ngx_memcpy(p, cursor->path.data, base);
            ngx_free(cursor->path.data);
        }
        cursor->path.data = p;
        cursor->allocated = capacity;
    }

    p = cursor->path.data + base;
    if (cursor->top != NULL) {
        *p++ = '/';
    }
    ngx_memcpy(p, name, len);
    p[len] = '\0';
    cursor->path.len = size;

    return NGX_OK;
}


/* A frame owns only a directory handle and its prefix length. */
static ngx_int_t
ngx_http_cache_purge_walk_open(ngx_http_cache_purge_cursor_t *cursor)
{
    ngx_http_cache_purge_dir_t  *frame;
    ngx_err_t                   err;
    ngx_int_t                   rc;
    const char                 *failed_op;

    frame = ngx_alloc(sizeof(*frame), cursor->ctx->log);
    if (frame == NULL) {
        cursor->ctx->scan_errors++;
        return NGX_ABORT;
    }

    rc = ngx_http_cache_purge_open_dir(&cursor->path,
                   cursor->top != NULL ? &cursor->top->dir : NULL, &frame->dir,
                   &failed_op);
    if (rc != NGX_OK) {
        err = ngx_errno;
        if (rc == NGX_ERROR && err != NGX_ENOENT) {
            cursor->ctx->scan_errors++;
            ngx_log_error(NGX_LOG_CRIT, cursor->ctx->log, err,
                          "ngx_cache_purge: %s \"%V\" failed",
                          failed_op, &cursor->path);
        }
        ngx_free(frame);
        return rc;
    }

    frame->len = cursor->path.len;
    frame->parent = cursor->top;
    cursor->top = frame;

    return NGX_OK;
}


void
ngx_http_cache_purge_walk_close(ngx_http_cache_purge_cursor_t *cursor)
{
    ngx_http_cache_purge_dir_t  *frame;

    frame = cursor->top;
    cursor->path.len = frame->len;
    cursor->path.data[frame->len] = '\0';
    if (ngx_close_dir(&frame->dir) == NGX_ERROR) {
        cursor->ctx->scan_errors++;
        ngx_log_error(NGX_LOG_CRIT, cursor->ctx->log, ngx_errno,
                      "ngx_cache_purge: " ngx_close_dir_n " \"%V\" failed",
                      &cursor->path);
    }

    cursor->top = frame->parent;
    ngx_free(frame);
}


/* A zero limit completes the walk; timed calls stop between slices. */
ngx_int_t
ngx_http_cache_purge_walk_step(ngx_http_cache_purge_cursor_t *cursor,
    ngx_uint_t limit)
{
    ngx_http_cache_purge_walk_ctx_t  *ctx;
    ngx_http_cache_purge_dir_t       *frame;
    ngx_int_t                        rc;
    ngx_err_t                        err;
    ngx_msec_t                       start;
    ngx_uint_t                       n;
    size_t                           len;
    u_char                          *name;
    const char                      *failed_op;

    ctx = cursor->ctx;
    start = ngx_current_msec;
    n = 0;

    if (!cursor->started) {
        cursor->started = 1;
        ctx->scan_errors = 0;
        if (ngx_http_cache_purge_walk_name(cursor, ctx->cache_root.data,
                                           ctx->cache_root.len) != NGX_OK)
        {
            ctx->scan_errors++;
            return NGX_ABORT;
        }
        rc = ngx_http_cache_purge_walk_open(cursor);
        if (rc != NGX_OK) {
            return rc;
        }
    }

    while (cursor->top != NULL) {
        if (limit && n++ != 0) {
            ngx_time_update();
            if (n > limit
                || (ngx_msec_int_t) (ngx_current_msec - start)
                   >= NGX_CACHE_PURGE_WALK_MSEC)
            {
                return NGX_AGAIN;
            }
        }

        frame = cursor->top;
        ngx_set_errno(0);
        if (ngx_read_dir(&frame->dir) == NGX_ERROR) {
            err = ngx_errno;
            if (err != NGX_ENOMOREFILES) {
                ctx->scan_errors++;
                cursor->path.len = frame->len;
                cursor->path.data[frame->len] = '\0';
                ngx_log_error(NGX_LOG_CRIT, ctx->log, err,
                              "ngx_cache_purge: " ngx_read_dir_n
                              " \"%V\" failed", &cursor->path);
            }
            ngx_http_cache_purge_walk_close(cursor);
            continue;
        }

        len = ngx_de_namelen(&frame->dir);
        name = ngx_de_name(&frame->dir);
        if ((len == 1 && name[0] == '.')
            || (len == 2 && name[0] == '.' && name[1] == '.'))
        {
            continue;
        }

        if (ngx_http_cache_purge_walk_name(cursor, name, len) != NGX_OK) {
            ctx->scan_errors++;
            goto failed;
        }

        rc = ngx_http_cache_purge_de_info(&cursor->path, &frame->dir,
                                         &failed_op);
        if (rc != NGX_OK) {
            if (rc == NGX_ERROR) {
                err = ngx_errno;
                if (err != NGX_ENOENT) {
                    ctx->scan_errors++;
                    ngx_log_error(NGX_LOG_CRIT, ctx->log, err,
                                  "ngx_cache_purge: %s \"%V\" failed",
                                  failed_op, &cursor->path);
                }
            }
            continue;
        }

        if (ngx_de_is_file(&frame->dir)) {
            rc = ngx_http_cache_purge_file_info(&cursor->path, &frame->dir,
                                               &failed_op);
            if (rc == NGX_OK) {
                ctx->file_size = ngx_de_size(&frame->dir);
                ctx->parent_dir = &frame->dir;
                if (ctx->visit != NULL) {
                    ctx->visit(ctx, &cursor->path);
                } else {
                    ngx_http_cache_purge_delete_file(ctx, &cursor->path);
                }
                ctx->parent_dir = NULL;
            } else if (rc == NGX_ERROR && ngx_errno != NGX_ENOENT) {
                ctx->scan_errors++;
                ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                              "ngx_cache_purge: %s \"%V\" failed",
                              failed_op, &cursor->path);
            }

        } else if (ngx_de_is_dir(&frame->dir)
                   && ngx_http_cache_purge_skip_protected_dir(ctx, &cursor->path)
                      == NGX_OK)
        {
            if (ngx_http_cache_purge_walk_open(cursor) == NGX_ABORT) {
                goto failed;
            }
        }
    }

    return ctx->scan_errors ? NGX_ERROR : NGX_OK;

failed:

    while (cursor->top != NULL) {
        ngx_http_cache_purge_walk_close(cursor);
    }

    return NGX_ABORT;
}


ngx_int_t
ngx_http_cache_purge_walk(ngx_http_cache_purge_walk_ctx_t *ctx)
{
    ngx_http_cache_purge_cursor_t  cursor;
    ngx_int_t                      rc;

    ngx_memzero(&cursor, sizeof(ngx_http_cache_purge_cursor_t));
    cursor.ctx = ctx;
    rc = ngx_http_cache_purge_walk_step(&cursor, 0);
    if (cursor.path.data != NULL) {
        ngx_free(cursor.path.data);
    }

    return rc;
}


ngx_uint_t
ngx_http_cache_purge_is_protected_path(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path)
{
    ngx_path_t      **paths;
    const ngx_str_t  *name;
    size_t            root_len;
    ngx_uint_t        j, n;

    if (path->data == NULL || ctx->cache_root.data == NULL
        || !ngx_http_cache_purge_path_contains(path, &ctx->cache_root))
    {
        return 1;
    }

    root_len = ngx_http_cache_purge_path_length(&ctx->cache_root);

    paths = ctx->paths != NULL ? ctx->paths->elts : NULL;
    n = ctx->protection != NULL ? ctx->protection->nelts
                               : (ctx->paths != NULL ? ctx->paths->nelts : 0);

    for (j = 0; j < n; j++) {
        if (ctx->protection != NULL) {
            name = &ctx->protection->names[j];

        } else {
            if (paths[j]->manager != NULL || paths[j]->loader != NULL) {
                continue;
            }
            name = &paths[j]->name;
        }

        if (name->len > root_len
            && ngx_http_cache_purge_path_contains(name, &ctx->cache_root)
            && ngx_http_cache_purge_path_contains(path, name))
        {
            return 1;
        }
    }

    return 0;
}


static ngx_int_t
ngx_http_cache_purge_skip_protected_dir(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path)
{
    if (ngx_http_cache_purge_is_protected_path(ctx, path)) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->log, 0,
                       "ngx_cache_purge: skipping protected directory \"%V\"",
                       path);
        return NGX_DECLINED;
    }

    return NGX_OK;
}

/* Cache filenames have 32 hex digits; temporary filenames do not. */
ngx_uint_t
ngx_http_cache_purge_is_cache_file(ngx_str_t *path)
{
    size_t  i, start;
    u_char  c;

    if (path->len < 32) {
        return 0;
    }

    start = path->len - 32;
    if (start != 0
        && !ngx_http_cache_purge_path_separator(path->data[start - 1]))
    {
        return 0;
    }

    for (i = start; i < path->len; i++) {
        c = path->data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
              || (c >= 'A' && c <= 'F')))
        {
            return 0;
        }
    }

    return 1;
}


/*
 * One bounded reader for every consumer.  Offsets come from the native cache
 * header, not a guessed URL boundary.  Never interpret another format/version
 * as a key, and never allocate memory from a size supplied by the file.
 */
ssize_t
ngx_http_cache_purge_read_key(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path, size_t len)
{
    ngx_file_t                      file;
    ngx_file_info_t                 info;
    ngx_http_file_cache_header_t   *h;
    ngx_md5_t                       md5;
    u_char                          marker[NGX_CACHE_PURGE_KEY_HDR_OFFSET];
    u_char                          hash[NGX_HTTP_CACHE_KEY_LEN], hex[32];
    ssize_t                         n, result;
    size_t                          size;
    const char                     *failed_op;

    ctx->entry_key_len = 0;
    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = *path;
    file.log = ctx->log;
    file.fd = ngx_http_cache_purge_open_entry(path, ctx->parent_dir, &failed_op);
    if (file.fd == NGX_INVALID_FILE) {
        result = ngx_errno == NGX_ENOENT ? NGX_DECLINED : NGX_ERROR;
        ngx_http_cache_purge_open_failed(ctx->log, path->data, failed_op);
        return result;
    }

    result = NGX_DECLINED;
    if (ngx_fd_info(file.fd, &info) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                      "ngx_cache_purge: " ngx_fd_info_n " \"%V\" failed",
                      path);
        result = NGX_ERROR;
        goto done;
    }
    if (!ngx_is_file(&info)) {
        goto done;
    }

    h = &ctx->header;
    n = ngx_read_file(&file, (u_char *) h, sizeof(*h), 0);
    if (n != (ssize_t) sizeof(*h)) {
        if (n == NGX_ERROR) {
            result = NGX_ERROR;
        }
        goto done;
    }

    size = sizeof(*h) + sizeof(marker);
    if (h->header_start < size + 1
        || h->header_start - size - 1 > NGX_CACHE_PURGE_ENTRY_KEY_MAX
        || h->body_start < h->header_start
        || (off_t) h->body_start > ngx_file_size(&info)
#ifdef NGX_HTTP_CACHE_VERSION
        || h->version != NGX_HTTP_CACHE_VERSION
#endif
#ifdef NGX_HTTP_CACHE_ETAG_LEN
        || h->etag_len > NGX_HTTP_CACHE_ETAG_LEN
#endif
#ifdef NGX_HTTP_CACHE_VARY_LEN
        || h->vary_len > NGX_HTTP_CACHE_VARY_LEN
#endif
       )
    {
        goto done;
    }

    n = ngx_read_file(&file, marker, sizeof(marker), sizeof(*h));
    if (n != sizeof(marker) || ngx_memcmp(marker, "\nKEY: ", sizeof(marker))) {
        if (n == NGX_ERROR) {
            result = NGX_ERROR;
        }
        goto done;
    }

    ctx->entry_key_len = h->header_start - size - 1;
    n = ngx_read_file(&file, ctx->key_buffer, ctx->entry_key_len + 1, size);
    if (n != (ssize_t) ctx->entry_key_len + 1
        || ctx->key_buffer[ctx->entry_key_len] != LF
        || ngx_crc32_long(ctx->key_buffer, ctx->entry_key_len) != h->crc32)
    {
        if (n == NGX_ERROR) {
            result = NGX_ERROR;
        }
        goto done;
    }

    /* Old nginx headers select OpenSSL's MD5 ABI. Keep that exact core ABI
     * when compiling an old nginx with OpenSSL 3; newer nginx uses its own. */
#if (NGX_OPENSSL_MD5) && (defined(__clang__) || (__GNUC__ > 4) \
    || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    ngx_md5_init(&md5);
    ngx_md5_update(&md5, ctx->key_buffer, ctx->entry_key_len);
    ngx_md5_final(hash, &md5);
#if (NGX_OPENSSL_MD5) && (defined(__clang__) || (__GNUC__ > 4) \
    || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#pragma GCC diagnostic pop
#endif
    ngx_hex_dump(hex, hash, sizeof(hash));
    if (path->len < sizeof(hex)
        || ngx_strncasecmp(path->data + path->len - sizeof(hex), hex,
                           sizeof(hex)))
    {
#ifdef NGX_HTTP_CACHE_VARY_LEN
        ngx_hex_dump(hex, h->variant, sizeof(h->variant));
        if (h->vary_len == 0 || path->len < sizeof(hex)
            || ngx_strncasecmp(path->data + path->len - sizeof(hex), hex,
                               sizeof(hex)))
#endif
        {
            goto done;
        }
    }

    ctx->file_size = ngx_file_size(&info);
    result = (ssize_t) ngx_min(len, ctx->entry_key_len + 1);

done:

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        result = NGX_ERROR;
        ngx_log_error(NGX_LOG_ALERT, ctx->log, ngx_errno,
                      "ngx_cache_purge: " ngx_close_file_n " \"%s\" failed",
                      path->data);
    }

    if (result < 0) {
        ctx->entry_key_len = 0;
    }
    return result;
}


/*
 * Decode the filename MD5 with a native-width, native-endian tree key.
 * Compare the remaining bytes in the same order as the cache rbtree.
 * The cache lock and the pending-task lock are never nested.
 */
static void
ngx_http_cache_purge_invalidate_node(ngx_http_file_cache_t *cache,
    ngx_str_t *path)
{
    ngx_rbtree_node_t           *node;
    ngx_rbtree_node_t           *sentinel;
    ngx_http_file_cache_node_t  *fcn;
    ngx_rbtree_key_t             node_key;
    u_char                       hash[NGX_HTTP_CACHE_KEY_LEN];
    u_char                      *p;
    ngx_uint_t                   i;
    ngx_int_t                    n;
    int                          cmp;

    /* Decode the final 32 hex digits as the cache key. */
    if (path->len < 32) {
        return;
    }

    p = path->data + path->len - 32;

    for (i = 0; i < NGX_HTTP_CACHE_KEY_LEN; i++) {
        n = ngx_hextoi(p + i * 2, 2);
        if (n == NGX_ERROR) {
            return;
        }
        hash[i] = (u_char) n;
    }

    /* Match the native byte order of the cache rbtree key. */
    ngx_memcpy(&node_key, hash, sizeof(ngx_rbtree_key_t));

    ngx_shmtx_lock(&cache->shpool->mutex);

    node     = cache->sh->rbtree.root;
    sentinel = cache->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (node_key < node->key) {
            node = node->left;
            continue;
        }

        if (node_key > node->key) {
            node = node->right;
            continue;
        }

        /* The native-width prefix matches; compare the remaining bytes. */
        fcn = (ngx_http_file_cache_node_t *) node;
        cmp = ngx_memcmp(hash + sizeof(ngx_rbtree_key_t),
                         fcn->key,
                         NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));

        if (cmp < 0) {
            node = node->left;
            continue;
        }

        if (cmp > 0) {
            node = node->right;
            continue;
        }

        /* Exact 16-byte match -- clear the node's accounting fields */
        if (fcn->exists) {
#if (nginx_version >= 1000001)
            cache->sh->size -= fcn->fs_size;
            fcn->fs_size     = 0;
#else
            cache->sh->size -= (fcn->length + cache->bsize - 1) / cache->bsize;
            fcn->length       = 0;
#endif
            fcn->exists = 0;
        }

        break;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);
}

/* Delete Vary variants after the primary entry has been invalidated. */
ngx_int_t
ngx_http_cache_purge_delete_variants(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_http_cache_purge_walk_ctx_t *ctx)
{
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t *key;

    key = r->cache->keys.elts;
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    ngx_http_cache_purge_walk_init(ctx, &cache->path->name,
                                   cmcf->protection, cmcf->paths, ngx_cycle->log);
    if (key[0].len == 0) {
        return NGX_OK;
    }
    ctx->key_partial = key[0].data;
    ctx->key_len = key[0].len;
    ctx->cache = cache;
    return ngx_http_cache_purge_walk(ctx);
}


/* Share path checks, fixed key storage and deletion across all walk modes. */
static void
ngx_http_cache_purge_delete_file(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path)
{
    size_t     len;
    ssize_t    n;
    ngx_int_t  rc;
    ngx_str_t  entry;

    len = ctx->key_len;

    if (ngx_http_cache_purge_is_protected_path(ctx, path)
        || !ngx_http_cache_purge_is_cache_file(path)
        || len >= NGX_CACHE_PURGE_KEY_MAX_LEN
        || (ctx->cache != NULL && len == 0))
    {
        return;
    }

    if (ctx->cache != NULL) {
        len++;
    }
    n = ngx_http_cache_purge_read_key(ctx, path, len);
    if (n != (ssize_t) len) {
        if (n == NGX_ERROR) {
            ctx->scan_errors++;
        }
        return;
    }

    if (ctx->namespace != NULL) {
        entry.data = ctx->key_buffer;
        entry.len = ctx->entry_key_len;
        if (ngx_http_cache_purge_generation_stale(ctx->namespace, &entry)
            != NGX_OK)
        {
            return;
        }
    }

    if (len != 0) {
        if (ctx->cache != NULL) {
            /* Exact keys retain case and end at the next newline. */
            if (ctx->key_buffer[ctx->key_len] != '\n'
                || ngx_memcmp(ctx->key_buffer, ctx->key_partial,
                              ctx->key_len) != 0)
            {
                return;
            }
            /* Invalidate the native cache node before deleting its file. */
            ngx_http_cache_purge_invalidate_node(ctx->cache, path);

        } else if (ngx_strncasecmp(ctx->key_buffer, ctx->key_partial,
                                   len) != 0)
        {
            return;
        }
    }

    rc = ngx_http_cache_purge_unlink(ctx->log, path->data, ctx->parent_dir);
    if (rc == NGX_OK) {
        if (ctx->files_deleted != (ngx_uint_t) -1) {
            ctx->files_deleted++;
        }
        if (ctx->file_size > 0) {
            ctx->bytes_deleted += ngx_min((uint64_t) ctx->file_size,
                                         (uint64_t) -1 - ctx->bytes_deleted);
        }
    } else if (rc == NGX_ERROR && ctx->delete_errors != (ngx_uint_t) -1) {
        ctx->delete_errors++;
    }
}


#endif
