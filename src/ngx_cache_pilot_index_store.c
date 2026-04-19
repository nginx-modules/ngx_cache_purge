#include "ngx_cache_pilot_index_store_internal.h"

#if (NGX_LINUX)

static ngx_http_cache_index_store_runtime_t ngx_http_cache_index_store_runtime;
static ngx_http_cache_index_store_ops_t ngx_http_cache_index_store_shm_ops;

static ngx_int_t ngx_http_cache_index_push_unique(ngx_pool_t *pool,
        ngx_array_t *tags, u_char *data, size_t len);
static ngx_int_t ngx_http_cache_index_path_known(ngx_str_t *path);
static ngx_int_t ngx_http_cache_index_parse_file(ngx_pool_t *pool,
        ngx_str_t *path, ngx_array_t *headers, ngx_array_t **tags,
        ngx_str_t *cache_key_text, time_t *mtime, off_t *size, ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_http_cache_index_store_t *ngx_http_cache_index_store_shm_open(
    ngx_http_cache_pilot_main_conf_t *pmcf, ngx_flag_t readonly,
    ngx_log_t *log);
static void ngx_http_cache_index_store_shm_close(
    ngx_http_cache_index_store_t *store);
static ngx_int_t ngx_http_cache_index_store_shm_begin_batch(
    ngx_http_cache_index_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_commit_batch(
    ngx_http_cache_index_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_rollback_batch(
    ngx_http_cache_index_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_upsert_file_meta(
    ngx_http_cache_index_store_t *store, ngx_str_t *zone_name,
    ngx_str_t *path, ngx_str_t *cache_key_text, time_t mtime, off_t size,
    ngx_array_t *tags, ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_delete_file(
    ngx_http_cache_index_store_t *store, ngx_str_t *zone_name,
    ngx_str_t *path, ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_collect_paths_by_tags(
    ngx_http_cache_index_store_t *store, ngx_pool_t *pool,
    ngx_str_t *zone_name, ngx_array_t *tags, ngx_array_t **paths,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_collect_paths_by_exact_key(
    ngx_http_cache_index_store_t *store, ngx_pool_t *pool,
    ngx_str_t *zone_name, ngx_str_t *key_text, ngx_array_t **paths,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_collect_paths_by_key_prefix(
    ngx_http_cache_index_store_t *store, ngx_pool_t *pool,
    ngx_str_t *zone_name, ngx_str_t *prefix, ngx_array_t **paths,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_get_zone_state(
    ngx_http_cache_index_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_index_zone_state_t *state, ngx_log_t *log);
static ngx_int_t ngx_http_cache_index_store_shm_set_zone_state(
    ngx_http_cache_index_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_index_zone_state_t *state, ngx_log_t *log);
static ngx_http_cache_index_shm_zone_t *ngx_http_cache_index_store_shm_lookup_zone(
    ngx_http_cache_index_store_shctx_t *sh, ngx_str_t *zone_name);
static ngx_http_cache_index_shm_zone_t *ngx_http_cache_index_store_shm_get_zone_locked(
    ngx_http_cache_index_store_ctx_t *ctx, ngx_str_t *zone_name,
    ngx_flag_t create);
static ngx_http_cache_index_shm_file_t *ngx_http_cache_index_store_shm_lookup_file(
    ngx_http_cache_index_shm_zone_t *zone, ngx_str_t *path);
static ngx_int_t ngx_http_cache_index_store_shm_remove_file_locked(
    ngx_http_cache_index_store_ctx_t *ctx, ngx_http_cache_index_shm_zone_t *zone,
    ngx_http_cache_index_shm_file_t *file);
static ngx_int_t ngx_http_cache_index_store_shm_push_path_unique(
    ngx_pool_t *pool, ngx_array_t *paths, u_char *data, size_t len);
static ngx_uint_t ngx_http_cache_index_store_shm_file_matches_tags(
    ngx_http_cache_index_shm_file_t *file, ngx_array_t *tags);
static void *ngx_http_cache_index_store_shm_alloc_locked(
    ngx_http_cache_index_store_ctx_t *ctx, size_t size);
static u_char *ngx_http_cache_index_store_shm_file_path(
    ngx_http_cache_index_shm_file_t *file);
static u_char *ngx_http_cache_index_store_shm_file_key(
    ngx_http_cache_index_shm_file_t *file);
static u_char *ngx_http_cache_index_store_shm_file_tag_at(
    ngx_http_cache_index_shm_file_t *file, ngx_uint_t index,
    size_t *len);

static ngx_http_cache_index_store_ops_t ngx_http_cache_index_store_shm_ops = {
    ngx_http_cache_index_store_shm_close,
    ngx_http_cache_index_store_shm_begin_batch,
    ngx_http_cache_index_store_shm_commit_batch,
    ngx_http_cache_index_store_shm_rollback_batch,
    ngx_http_cache_index_store_shm_upsert_file_meta,
    ngx_http_cache_index_store_shm_delete_file,
    ngx_http_cache_index_store_shm_collect_paths_by_tags,
    ngx_http_cache_index_store_shm_collect_paths_by_exact_key,
    ngx_http_cache_index_store_shm_collect_paths_by_key_prefix,
    ngx_http_cache_index_store_shm_get_zone_state,
    ngx_http_cache_index_store_shm_set_zone_state
};

ngx_int_t
ngx_http_cache_index_store_init_conf(ngx_conf_t *cf,
                     ngx_http_cache_pilot_main_conf_t *pmcf) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_str_t                          zone_name = ngx_string("cache_pilot_index");

    if (pmcf == NULL || pmcf->backend != NGX_HTTP_CACHE_TAG_BACKEND_SHM) {
    return NGX_OK;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_index_store_ctx_t));
    if (ctx == NULL) {
    return NGX_ERROR;
    }

    pmcf->index_zone = ngx_shared_memory_add(cf, &zone_name,
               pmcf->index_shm_size,
               &ngx_http_cache_pilot_module);
    if (pmcf->index_zone == NULL) {
    return NGX_ERROR;
    }

    pmcf->index_zone->init = ngx_http_cache_index_store_init_zone;
    pmcf->index_zone->data = ctx;

    return NGX_OK;
}

ngx_flag_t
ngx_http_cache_index_store_configured(ngx_http_cache_pilot_main_conf_t *pmcf) {
    return pmcf != NULL
       && pmcf->backend == NGX_HTTP_CACHE_TAG_BACKEND_SHM
       && pmcf->index_zone != NULL;
}

ngx_http_cache_index_store_t *
ngx_http_cache_index_store_open_writer(ngx_http_cache_pilot_main_conf_t *pmcf,
                                       ngx_log_t *log) {
    if (!ngx_http_cache_index_store_configured(pmcf)) {
        return NULL;
    }

    return ngx_http_cache_index_store_shm_open(pmcf, 0, log);
}

ngx_http_cache_index_store_t *
ngx_http_cache_index_store_open_reader(ngx_http_cache_pilot_main_conf_t *pmcf,
                                       ngx_log_t *log) {
    if (!ngx_http_cache_index_store_configured(pmcf)) {
        return NULL;
    }

    return ngx_http_cache_index_store_shm_open(pmcf, 1, log);
}

void
ngx_http_cache_index_store_close(ngx_http_cache_index_store_t *store) {
    if (store == NULL) {
        return;
    }

    /* Close the backing resource (socket / db handle).  The store struct
     * itself is allocated from cycle->pool and freed automatically when
     * the pool is destroyed on worker exit or config reload. */
    store->ops->close(store);
}

ngx_int_t
ngx_http_cache_index_store_begin_batch(ngx_http_cache_index_store_t *store,
                                       ngx_log_t *log) {
    return store->ops->begin_batch(store, log);
}

ngx_int_t
ngx_http_cache_index_store_commit_batch(ngx_http_cache_index_store_t *store,
                                        ngx_log_t *log) {
    return store->ops->commit_batch(store, log);
}

ngx_int_t
ngx_http_cache_index_store_rollback_batch(ngx_http_cache_index_store_t *store,
        ngx_log_t *log) {
    return store->ops->rollback_batch(store, log);
}

ngx_int_t
ngx_http_cache_index_store_upsert_file_meta(ngx_http_cache_index_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, ngx_str_t *cache_key_text,
        time_t mtime, off_t size, ngx_array_t *tags, ngx_log_t *log) {
    return store->ops->upsert_file_meta(store, zone_name, path, cache_key_text,
                                        mtime, size, tags, log);
}

ngx_int_t
ngx_http_cache_index_store_collect_paths_by_exact_key(
    ngx_http_cache_index_store_t *store, ngx_pool_t *pool,
    ngx_str_t *zone_name, ngx_str_t *key_text,
    ngx_array_t **paths, ngx_log_t *log) {
    return store->ops->collect_paths_by_exact_key(store, pool, zone_name,
            key_text, paths, log);
}

ngx_int_t
ngx_http_cache_index_store_collect_paths_by_key_prefix(
    ngx_http_cache_index_store_t *store, ngx_pool_t *pool,
    ngx_str_t *zone_name, ngx_str_t *prefix,
    ngx_array_t **paths, ngx_log_t *log) {
    return store->ops->collect_paths_by_key_prefix(store, pool, zone_name,
            prefix, paths, log);
}

ngx_int_t
ngx_http_cache_index_store_delete_file(ngx_http_cache_index_store_t *store,
                                       ngx_str_t *zone_name, ngx_str_t *path,
                                       ngx_log_t *log) {
    return store->ops->delete_file(store, zone_name, path, log);
}

ngx_int_t
ngx_http_cache_index_store_collect_paths_by_tags(ngx_http_cache_index_store_t *store,
        ngx_pool_t *pool, ngx_str_t *zone_name, ngx_array_t *tags,
        ngx_array_t **paths, ngx_log_t *log) {
    return store->ops->collect_paths_by_tags(store, pool, zone_name, tags, paths,
            log);
}

ngx_int_t
ngx_http_cache_index_store_get_zone_state(ngx_http_cache_index_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_index_zone_state_t *state,
        ngx_log_t *log) {
    return store->ops->get_zone_state(store, zone_name, state, log);
}

ngx_int_t
ngx_http_cache_index_store_set_zone_state(ngx_http_cache_index_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_index_zone_state_t *state,
        ngx_log_t *log) {
    return store->ops->set_zone_state(store, zone_name, state, log);
}

ngx_int_t
ngx_http_cache_index_store_process_file(ngx_http_cache_index_store_t *store,
                                        ngx_str_t *zone_name, ngx_str_t *path,
                                        ngx_array_t *headers,
                                        ngx_log_t *log) {
    ngx_pool_t   *pool;
    ngx_array_t  *tags;
    ngx_str_t     cache_key_text;
    time_t        mtime;
    off_t         size;
    ngx_int_t     parse_rc;
    ngx_int_t     rc;

    pool = ngx_create_pool(4096, log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    ngx_str_null(&cache_key_text);

    if (headers == NULL || headers->nelts == 0) {
        /* No configured tag headers: still parse for KEY: so the key index
         * is populated even when tag indexing is not in use. */
        parse_rc = ngx_http_cache_index_parse_file(pool, path, NULL, &tags,
                   &cache_key_text, &mtime, &size,
                   log);
        if (parse_rc != NGX_OK) {
            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, log, 0,
                           "cache_tag process_file skipped zone:\"%V\" path:\"%V\" rc:%i",
                           zone_name, path, parse_rc);
            ngx_destroy_pool(pool);
            return ngx_http_cache_index_store_delete_file(store, zone_name, path,
                    log);
        }

        ngx_log_debug5(NGX_LOG_DEBUG_HTTP, log, 0,
                       "cache_tag process_file zone:\"%V\" path:\"%V\" key_len:%uz tags:%ui headers:%ui",
                       zone_name, path, cache_key_text.len,
                       tags != NULL ? tags->nelts : 0,
                       headers != NULL ? headers->nelts : 0);

        rc = ngx_http_cache_index_store_upsert_file_meta(store, zone_name, path,
                &cache_key_text, mtime, size, NULL, log);
        ngx_destroy_pool(pool);
        return rc;
    }

    parse_rc = ngx_http_cache_index_parse_file(pool, path, headers, &tags,
               &cache_key_text, &mtime, &size,
               log);
    if (parse_rc != NGX_OK) {
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, log, 0,
                       "cache_tag process_file skipped zone:\"%V\" path:\"%V\" rc:%i",
                       zone_name, path, parse_rc);
        ngx_destroy_pool(pool);
        return ngx_http_cache_index_store_delete_file(store, zone_name, path, log);
    }

    ngx_log_debug5(NGX_LOG_DEBUG_HTTP, log, 0,
                   "cache_tag process_file zone:\"%V\" path:\"%V\" key_len:%uz tags:%ui headers:%ui",
                   zone_name, path, cache_key_text.len,
                   tags != NULL ? tags->nelts : 0,
                   headers != NULL ? headers->nelts : 0);

    rc = ngx_http_cache_index_store_upsert_file_meta(store, zone_name, path,
            &cache_key_text, mtime, size, tags, log);
    ngx_destroy_pool(pool);

    return rc;
}

ngx_int_t
ngx_http_cache_index_store_runtime_init(ngx_cycle_t *cycle,
                                        ngx_http_cache_pilot_main_conf_t *pmcf,
                                        ngx_flag_t owner) {
    ngx_memzero(&ngx_http_cache_index_store_runtime,
                sizeof(ngx_http_cache_index_store_runtime));

    ngx_http_cache_index_store_runtime.cycle = cycle;
    ngx_http_cache_index_store_runtime.owner = owner;

    if (!ngx_http_cache_index_store_configured(pmcf)) {
        return NGX_OK;
    }

    ngx_http_cache_index_store_runtime.reader =
        ngx_http_cache_index_store_open_reader(pmcf, cycle->log);
    if (ngx_http_cache_index_store_runtime.reader == NULL) {
        return NGX_ERROR;
    }

    if (owner) {
        ngx_http_cache_index_store_runtime.writer =
            ngx_http_cache_index_store_runtime.reader;
    }

    return NGX_OK;
}

void
ngx_http_cache_index_store_runtime_shutdown(void) {
    ngx_http_cache_index_store_close(ngx_http_cache_index_store_runtime.writer);
    ngx_http_cache_index_store_close(ngx_http_cache_index_store_runtime.reader);

    ngx_memzero(&ngx_http_cache_index_store_runtime,
                sizeof(ngx_http_cache_index_store_runtime));
}

ngx_http_cache_index_store_t *
ngx_http_cache_index_store_writer(void) {
    return ngx_http_cache_index_store_runtime.writer;
}

ngx_http_cache_index_store_t *
ngx_http_cache_index_store_reader(ngx_http_cache_pilot_main_conf_t *pmcf,
                                  ngx_log_t *log) {
    if (ngx_http_cache_index_store_runtime.reader != NULL) {
        return ngx_http_cache_index_store_runtime.reader;
    }

    ngx_http_cache_index_store_runtime.reader =
        ngx_http_cache_index_store_open_reader(pmcf, log);

    return ngx_http_cache_index_store_runtime.reader;
}

static ngx_int_t
ngx_http_cache_index_store_init_zone(ngx_shm_zone_t *shm_zone, void *data) {
    ngx_http_cache_index_store_ctx_t    *octx = data;
    ngx_http_cache_index_store_ctx_t    *ctx;
    ngx_http_cache_index_store_shctx_t  *sh;

    ctx = shm_zone->data;

    if (octx != NULL) {
        ctx->shpool = octx->shpool;
        ctx->sh = octx->sh;
        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;
        return NGX_OK;
    }

    sh = ngx_slab_calloc(ctx->shpool, sizeof(ngx_http_cache_index_store_shctx_t));
    if (sh == NULL) {
        return NGX_ERROR;
    }

    ngx_queue_init(&sh->zones);

    ctx->shpool->data = sh;
    ctx->sh = sh;

    return NGX_OK;
}

static ngx_http_cache_index_store_t *
ngx_http_cache_index_store_shm_open(ngx_http_cache_pilot_main_conf_t *pmcf,
                                    ngx_flag_t readonly, ngx_log_t *log) {
    ngx_http_cache_index_store_t      *store;
    ngx_http_cache_index_store_ctx_t  *ctx;

    (void) log;

    if (pmcf == NULL || pmcf->index_zone == NULL || pmcf->index_zone->data == NULL) {
        return NULL;
    }

    ctx = pmcf->index_zone->data;
    if (ctx->shpool == NULL || ctx->sh == NULL) {
        return NULL;
    }

    store = ngx_pcalloc(ngx_cycle->pool, sizeof(ngx_http_cache_index_store_t));
    if (store == NULL) {
        return NULL;
    }

    store->ops = &ngx_http_cache_index_store_shm_ops;
    store->backend = NGX_HTTP_CACHE_TAG_BACKEND_SHM;
    store->readonly = readonly;
    store->u.shm.ctx = ctx;
    store->u.shm.batch_locked = 0;

    return store;
}

static void
ngx_http_cache_index_store_shm_close(ngx_http_cache_index_store_t *store) {
    (void) store;
}

static ngx_int_t
ngx_http_cache_index_store_shm_begin_batch(ngx_http_cache_index_store_t *store,
                                           ngx_log_t *log) {
    (void) log;

    if (store->u.shm.batch_locked) {
        return NGX_OK;
    }

    ngx_shmtx_lock(&store->u.shm.ctx->shpool->mutex);
    store->u.shm.batch_locked = 1;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_commit_batch(ngx_http_cache_index_store_t *store,
                                            ngx_log_t *log) {
    (void) log;

    if (store->u.shm.batch_locked) {
        store->u.shm.batch_locked = 0;
        ngx_shmtx_unlock(&store->u.shm.ctx->shpool->mutex);
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_rollback_batch(ngx_http_cache_index_store_t *store,
                                              ngx_log_t *log) {
    return ngx_http_cache_index_store_shm_commit_batch(store, log);
}

static ngx_http_cache_index_shm_zone_t *
ngx_http_cache_index_store_shm_lookup_zone(ngx_http_cache_index_store_shctx_t *sh,
                                           ngx_str_t *zone_name) {
    ngx_queue_t                     *q;
    ngx_http_cache_index_shm_zone_t *zone;

    for (q = ngx_queue_head(&sh->zones);
         q != ngx_queue_sentinel(&sh->zones);
         q = ngx_queue_next(q)) {
        zone = ngx_queue_data(q, ngx_http_cache_index_shm_zone_t, queue);
        if (zone->name_len == zone_name->len
                && ngx_strncmp(zone->name, zone_name->data, zone_name->len) == 0) {
            return zone;
        }
    }

    return NULL;
}

static void *
ngx_http_cache_index_store_shm_alloc_locked(ngx_http_cache_index_store_ctx_t *ctx,
                                            size_t size) {
    void *p;

    p = ngx_slab_alloc_locked(ctx->shpool, size);
    if (p == NULL && ctx->sh != NULL) {
        ctx->sh->alloc_failures++;
    }

    return p;
}

static ngx_http_cache_index_shm_zone_t *
ngx_http_cache_index_store_shm_get_zone_locked(ngx_http_cache_index_store_ctx_t *ctx,
                                               ngx_str_t *zone_name,
                                               ngx_flag_t create) {
    ngx_http_cache_index_shm_zone_t *zone;
    size_t                           size;

    zone = ngx_http_cache_index_store_shm_lookup_zone(ctx->sh, zone_name);
    if (zone != NULL || !create) {
        return zone;
    }

    size = sizeof(ngx_http_cache_index_shm_zone_t) + zone_name->len;
    zone = ngx_http_cache_index_store_shm_alloc_locked(ctx, size);
    if (zone == NULL) {
        return NULL;
    }

    ngx_memzero(zone, sizeof(ngx_http_cache_index_shm_zone_t));
    zone->name_len = zone_name->len;
    ngx_memcpy(zone->name, zone_name->data, zone_name->len);
    zone->name[zone_name->len] = '\0';
    ngx_queue_init(&zone->files);
    ngx_queue_insert_tail(&ctx->sh->zones, &zone->queue);

    return zone;
}

static ngx_http_cache_index_shm_file_t *
ngx_http_cache_index_store_shm_lookup_file(ngx_http_cache_index_shm_zone_t *zone,
                                           ngx_str_t *path) {
    ngx_queue_t                     *q;
    ngx_http_cache_index_shm_file_t *file;

    for (q = ngx_queue_head(&zone->files);
         q != ngx_queue_sentinel(&zone->files);
         q = ngx_queue_next(q)) {
        file = ngx_queue_data(q, ngx_http_cache_index_shm_file_t, queue);
        if (file->path_len == path->len
                && ngx_strncmp(file->data, path->data, path->len) == 0) {
            return file;
        }
    }

    return NULL;
}

static ngx_int_t
ngx_http_cache_index_store_shm_remove_file_locked(ngx_http_cache_index_store_ctx_t *ctx,
                                                  ngx_http_cache_index_shm_zone_t *zone,
                                                  ngx_http_cache_index_shm_file_t *file) {
    (void) zone;

    ngx_queue_remove(&file->queue);
    ngx_slab_free_locked(ctx->shpool, file);

    return NGX_OK;
}

static u_char *
ngx_http_cache_index_store_shm_file_path(ngx_http_cache_index_shm_file_t *file) {
    return file->data;
}

static u_char *
ngx_http_cache_index_store_shm_file_key(ngx_http_cache_index_shm_file_t *file) {
    return file->data + file->path_len + 1;
}

static u_char *
ngx_http_cache_index_store_shm_file_tag_at(ngx_http_cache_index_shm_file_t *file,
                                           ngx_uint_t index, size_t *len) {
    u_char      *p;
    ngx_uint_t   i;

    p = ngx_http_cache_index_store_shm_file_key(file) + file->key_len + 1;
    for (i = 0; i < file->tag_count; i++) {
        if (i == index) {
            *len = ngx_strlen(p);
            return p;
        }

        p += ngx_strlen(p) + 1;
    }

    *len = 0;
    return NULL;
}

static ngx_int_t
ngx_http_cache_index_store_shm_upsert_file_meta(ngx_http_cache_index_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, ngx_str_t *cache_key_text,
        time_t mtime, off_t size, ngx_array_t *tags, ngx_log_t *log) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_http_cache_index_shm_zone_t   *zone;
    ngx_http_cache_index_shm_file_t   *file;
    ngx_str_t                         *tag;
    ngx_uint_t                         i;
    size_t                             alloc_size, tag_bytes, copied;
    u_char                            *p;
    ngx_flag_t                         locked;

    (void) log;

    ctx = store->u.shm.ctx;
    locked = 0;
    if (!store->u.shm.batch_locked) {
        ngx_shmtx_lock(&ctx->shpool->mutex);
        locked = 1;
    }

    zone = ngx_http_cache_index_store_shm_get_zone_locked(ctx, zone_name, 1);
    if (zone == NULL) {
        if (locked) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
        }
        return NGX_ERROR;
    }

    file = ngx_http_cache_index_store_shm_lookup_file(zone, path);
    if (file != NULL) {
        ngx_http_cache_index_store_shm_remove_file_locked(ctx, zone, file);
    }

    tag_bytes = 0;
    if (tags != NULL && tags->nelts > 0) {
        tag = tags->elts;
        for (i = 0; i < tags->nelts; i++) {
            tag_bytes += tag[i].len + 1;
        }
    }

    alloc_size = sizeof(ngx_http_cache_index_shm_file_t)
                 + path->len + 1
                 + cache_key_text->len + 1
                 + tag_bytes;

    file = ngx_http_cache_index_store_shm_alloc_locked(ctx, alloc_size);
    if (file == NULL) {
        if (locked) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
        }
        return NGX_ERROR;
    }

    ngx_memzero(file, sizeof(ngx_http_cache_index_shm_file_t));
    file->mtime = mtime;
    file->size = size;
    file->path_len = path->len;
    file->key_len = cache_key_text->len;
    file->tag_count = tags != NULL ? tags->nelts : 0;

    p = file->data;
    p = ngx_cpymem(p, path->data, path->len);
    *p++ = '\0';
    p = ngx_cpymem(p, cache_key_text->data, cache_key_text->len);
    *p++ = '\0';

    if (tags != NULL && tags->nelts > 0) {
        tag = tags->elts;
        copied = 0;
        for (i = 0; i < tags->nelts; i++) {
            p = ngx_cpymem(p, tag[i].data, tag[i].len);
            *p++ = '\0';
            copied += tag[i].len + 1;
        }

        (void) copied;
    }

    ngx_queue_insert_tail(&zone->files, &file->queue);

    if (locked) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_delete_file(ngx_http_cache_index_store_t *store,
                                           ngx_str_t *zone_name, ngx_str_t *path,
                                           ngx_log_t *log) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_http_cache_index_shm_zone_t   *zone;
    ngx_http_cache_index_shm_file_t   *file;
    ngx_flag_t                         locked;

    (void) log;

    ctx = store->u.shm.ctx;
    locked = 0;
    if (!store->u.shm.batch_locked) {
        ngx_shmtx_lock(&ctx->shpool->mutex);
        locked = 1;
    }

    zone = ngx_http_cache_index_store_shm_lookup_zone(ctx->sh, zone_name);
    if (zone != NULL) {
        file = ngx_http_cache_index_store_shm_lookup_file(zone, path);
        if (file != NULL) {
            ngx_http_cache_index_store_shm_remove_file_locked(ctx, zone, file);
        }
    }

    if (locked) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_push_path_unique(ngx_pool_t *pool,
                                                ngx_array_t *paths, u_char *data,
                                                size_t len) {
    ngx_str_t   *path;
    ngx_uint_t   i;

    path = paths->elts;
    for (i = 0; i < paths->nelts; i++) {
        if (path[i].len == len && ngx_strncmp(path[i].data, data, len) == 0) {
            return NGX_OK;
        }
    }

    path = ngx_array_push(paths);
    if (path == NULL) {
        return NGX_ERROR;
    }

    path->data = ngx_pnalloc(pool, len);
    if (path->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(path->data, data, len);
    path->len = len;

    return NGX_OK;
}

static ngx_uint_t
ngx_http_cache_index_store_shm_file_matches_tags(ngx_http_cache_index_shm_file_t *file,
                                                 ngx_array_t *tags) {
    ngx_str_t    *tag;
    ngx_uint_t    i, j;
    size_t        len;
    u_char       *value;

    if (tags == NULL || tags->nelts == 0 || file->tag_count == 0) {
        return 0;
    }

    tag = tags->elts;
    for (i = 0; i < file->tag_count; i++) {
        value = ngx_http_cache_index_store_shm_file_tag_at(file, i, &len);
        if (value == NULL) {
            continue;
        }

        for (j = 0; j < tags->nelts; j++) {
            if (tag[j].len == len && ngx_strncmp(tag[j].data, value, len) == 0) {
                return 1;
            }
        }
    }

    return 0;
}

static ngx_int_t
ngx_http_cache_index_store_shm_collect_paths_by_tags(ngx_http_cache_index_store_t *store,
        ngx_pool_t *pool, ngx_str_t *zone_name, ngx_array_t *tags,
        ngx_array_t **paths, ngx_log_t *log) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_http_cache_index_shm_zone_t   *zone;
    ngx_http_cache_index_shm_file_t   *file;
    ngx_queue_t                       *q;
    ngx_array_t                       *result;

    (void) log;

    result = ngx_array_create(pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    *paths = result;
    ctx = store->u.shm.ctx;

    ngx_shmtx_lock(&ctx->shpool->mutex);
    zone = ngx_http_cache_index_store_shm_lookup_zone(ctx->sh, zone_name);
    if (zone != NULL) {
        for (q = ngx_queue_head(&zone->files);
             q != ngx_queue_sentinel(&zone->files);
             q = ngx_queue_next(q)) {
            file = ngx_queue_data(q, ngx_http_cache_index_shm_file_t, queue);
            if (!ngx_http_cache_index_store_shm_file_matches_tags(file, tags)) {
                continue;
            }

            if (ngx_http_cache_index_store_shm_push_path_unique(pool, result,
                    ngx_http_cache_index_store_shm_file_path(file),
                    file->path_len) != NGX_OK) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
                return NGX_ERROR;
            }
        }
    }
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_collect_paths_by_exact_key(
        ngx_http_cache_index_store_t *store, ngx_pool_t *pool,
        ngx_str_t *zone_name, ngx_str_t *key_text, ngx_array_t **paths,
        ngx_log_t *log) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_http_cache_index_shm_zone_t   *zone;
    ngx_http_cache_index_shm_file_t   *file;
    ngx_queue_t                       *q;
    ngx_array_t                       *result;

    (void) log;

    result = ngx_array_create(pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    *paths = result;
    ctx = store->u.shm.ctx;

    ngx_shmtx_lock(&ctx->shpool->mutex);
    zone = ngx_http_cache_index_store_shm_lookup_zone(ctx->sh, zone_name);
    if (zone != NULL) {
        for (q = ngx_queue_head(&zone->files);
             q != ngx_queue_sentinel(&zone->files);
             q = ngx_queue_next(q)) {
            file = ngx_queue_data(q, ngx_http_cache_index_shm_file_t, queue);
            if (file->key_len != key_text->len
                    || ngx_strncmp(ngx_http_cache_index_store_shm_file_key(file),
                                   key_text->data, key_text->len) != 0) {
                continue;
            }

            if (ngx_http_cache_index_store_shm_push_path_unique(pool, result,
                    ngx_http_cache_index_store_shm_file_path(file),
                    file->path_len) != NGX_OK) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
                return NGX_ERROR;
            }
        }
    }
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_collect_paths_by_key_prefix(
        ngx_http_cache_index_store_t *store, ngx_pool_t *pool,
        ngx_str_t *zone_name, ngx_str_t *prefix, ngx_array_t **paths,
        ngx_log_t *log) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_http_cache_index_shm_zone_t   *zone;
    ngx_http_cache_index_shm_file_t   *file;
    ngx_queue_t                       *q;
    ngx_array_t                       *result;

    (void) log;

    result = ngx_array_create(pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    *paths = result;
    ctx = store->u.shm.ctx;

    ngx_shmtx_lock(&ctx->shpool->mutex);
    zone = ngx_http_cache_index_store_shm_lookup_zone(ctx->sh, zone_name);
    if (zone != NULL) {
        for (q = ngx_queue_head(&zone->files);
             q != ngx_queue_sentinel(&zone->files);
             q = ngx_queue_next(q)) {
            file = ngx_queue_data(q, ngx_http_cache_index_shm_file_t, queue);
            if (file->key_len < prefix->len
                    || ngx_strncmp(ngx_http_cache_index_store_shm_file_key(file),
                                   prefix->data, prefix->len) != 0) {
                continue;
            }

            if (ngx_http_cache_index_store_shm_push_path_unique(pool, result,
                    ngx_http_cache_index_store_shm_file_path(file),
                    file->path_len) != NGX_OK) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
                return NGX_ERROR;
            }
        }
    }
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_get_zone_state(ngx_http_cache_index_store_t *store,
        ngx_str_t *zone_name, ngx_http_cache_index_zone_state_t *state,
        ngx_log_t *log) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_http_cache_index_shm_zone_t   *zone;

    (void) log;

    state->bootstrap_complete = 0;
    state->last_bootstrap_at = 0;

    ctx = store->u.shm.ctx;
    ngx_shmtx_lock(&ctx->shpool->mutex);
    zone = ngx_http_cache_index_store_shm_lookup_zone(ctx->sh, zone_name);
    if (zone != NULL) {
        state->bootstrap_complete = zone->bootstrap_complete;
        state->last_bootstrap_at = zone->last_bootstrap_at;
    }
    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_store_shm_set_zone_state(ngx_http_cache_index_store_t *store,
        ngx_str_t *zone_name, ngx_http_cache_index_zone_state_t *state,
        ngx_log_t *log) {
    ngx_http_cache_index_store_ctx_t  *ctx;
    ngx_http_cache_index_shm_zone_t   *zone;
    ngx_flag_t                         locked;

    (void) log;

    ctx = store->u.shm.ctx;
    locked = 0;
    if (!store->u.shm.batch_locked) {
        ngx_shmtx_lock(&ctx->shpool->mutex);
        locked = 1;
    }

    zone = ngx_http_cache_index_store_shm_get_zone_locked(ctx, zone_name, 1);
    if (zone == NULL) {
        if (locked) {
            ngx_shmtx_unlock(&ctx->shpool->mutex);
        }
        return NGX_ERROR;
    }

    zone->bootstrap_complete = state->bootstrap_complete;
    zone->last_bootstrap_at = state->last_bootstrap_at;

    if (locked) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_index_extract_tokens(ngx_pool_t *pool, u_char *value, size_t len,
                                    ngx_array_t *tags, ngx_log_t *log) {
    size_t  i, start, end;

    i = 0;
    while (i < len) {
        while (i < len && (value[i] == ' ' || value[i] == '\t'
                           || value[i] == '\r' || value[i] == '\n'
                           || value[i] == ',')) {
            i++;
        }

        start = i;

        while (i < len && value[i] != ' ' && value[i] != '\t'
                && value[i] != '\r' && value[i] != '\n'
                && value[i] != ',') {
            i++;
        }

        end = i;
        if (end > start) {
            if (tags->nelts >= NGX_HTTP_CACHE_TAG_MAX_TAGS_PER_FILE) {
                ngx_log_error(NGX_LOG_WARN, log, 0,
                              "cache tag: too many tags in response header, "
                              "truncating at %d",
                              NGX_HTTP_CACHE_TAG_MAX_TAGS_PER_FILE);
                break;
            }

            if (ngx_http_cache_index_push_unique(pool, tags, value + start,
                                                 end - start) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_push_unique(ngx_pool_t *pool, ngx_array_t *tags,
                                 u_char *data, size_t len) {
    ngx_str_t   *tag;
    ngx_uint_t   i;

    if (len == 0) {
        return NGX_OK;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        if (tag[i].len == len
                && ngx_strncasecmp(tag[i].data, data, len) == 0) {
            return NGX_OK;
        }
    }

    tag = ngx_array_push(tags);
    if (tag == NULL) {
        return NGX_ERROR;
    }

    tag->data = ngx_pnalloc(pool, len);
    if (tag->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(tag->data, data, len);
    tag->len = len;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_index_path_known(ngx_str_t *path) {
    u_char  *p;

    p = path->data + path->len;
    while (p > path->data && p[-1] != '/') {
        p--;
    }

    return (size_t)(path->data + path->len - p) == 2 * NGX_HTTP_CACHE_KEY_LEN;
}

static ngx_int_t
ngx_http_cache_index_parse_file(ngx_pool_t *pool, ngx_str_t *path,
                                ngx_array_t *headers, ngx_array_t **tags,
                                ngx_str_t *cache_key_text,
                                time_t *mtime, off_t *size, ngx_log_t *log) {
    static u_char        key_hdr[] = "KEY:";
    ngx_file_info_t      fi;
    ngx_file_t           file;
    u_char              *buf;
    ssize_t              n;
    size_t               max_read, i, j, line_end, value_start;
    ngx_array_t         *result;
    ngx_str_t           *header;

    if (!ngx_http_cache_index_path_known(path)) {
        return NGX_DECLINED;
    }

    if (ngx_file_info(path->data, &fi) == NGX_FILE_ERROR) {
        return NGX_DECLINED;
    }

    max_read = (size_t) ngx_min((off_t) 65536, ngx_file_size(&fi));
    if (max_read == 0) {
        return NGX_DECLINED;
    }

    buf = ngx_pnalloc(pool, max_read);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = *path;
    file.log = log;
    file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (file.fd == NGX_INVALID_FILE) {
        return NGX_DECLINED;
    }

    n = ngx_read_file(&file, buf, max_read, 0);
    ngx_close_file(file.fd);

    if (n == NGX_ERROR || n <= 0) {
        return NGX_DECLINED;
    }

    result = ngx_array_create(pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < (size_t) n; i++) {
        if (i != 0 && buf[i - 1] != '\n') {
            continue;
        }

        /* Extract KEY: line (capture whole value, not token-split) */
        if (cache_key_text->len == 0
                && i + 4 <= (size_t) n
                && ngx_strncasecmp(buf + i, key_hdr, 4) == 0) {
            value_start = i + 4;
            while (value_start < (size_t) n
                    && (buf[value_start] == ' ' || buf[value_start] == '\t')) {
                value_start++;
            }
            line_end = value_start;
            while (line_end < (size_t) n
                    && buf[line_end] != '\n' && buf[line_end] != '\r') {
                line_end++;
            }
            if (line_end > value_start) {
                cache_key_text->data = ngx_pnalloc(pool,
                                                   line_end - value_start + 1);
                if (cache_key_text->data == NULL) {
                    return NGX_ERROR;
                }
                ngx_memcpy(cache_key_text->data, buf + value_start,
                           line_end - value_start);
                cache_key_text->len = line_end - value_start;
                cache_key_text->data[cache_key_text->len] = '\0';
            }
        }

        if (headers == NULL) {
            continue;
        }

        header = headers->elts;
        for (j = 0; j < headers->nelts; j++) {
            if (i + header[j].len + 1 >= (size_t) n) {
                continue;
            }

            if (ngx_strncasecmp(buf + i, header[j].data, header[j].len) != 0
                    || buf[i + header[j].len] != ':') {
                continue;
            }

            value_start = i + header[j].len + 1;
            while (value_start < (size_t) n
                    && (buf[value_start] == ' ' || buf[value_start] == '\t')) {
                value_start++;
            }

            line_end = value_start;
            while (line_end < (size_t) n && buf[line_end] != '\n'
                    && buf[line_end] != '\r') {
                line_end++;
            }

            if (ngx_http_cache_index_extract_tokens(pool, buf + value_start,
                                                    line_end - value_start,
                                                    result, log) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    *mtime = ngx_file_mtime(&fi);
    *size = ngx_file_size(&fi);
    *tags = result;

    return NGX_OK;
}

#endif
