#include "ngx_cache_purge_tag_store_internal.h"

#if (NGX_LINUX)

static ngx_http_cache_tag_store_runtime_t ngx_http_cache_tag_store_runtime;

static ngx_int_t ngx_http_cache_tag_push_unique(ngx_pool_t *pool,
        ngx_array_t *tags, u_char *data, size_t len);
static ngx_int_t ngx_http_cache_tag_path_known(ngx_str_t *path);
static ngx_int_t ngx_http_cache_tag_parse_file(ngx_pool_t *pool,
        ngx_str_t *path, ngx_array_t *headers, ngx_array_t **tags, time_t *mtime,
        off_t *size, ngx_log_t *log);

ngx_flag_t
ngx_http_cache_tag_store_configured(ngx_http_cache_purge_main_conf_t *pmcf) {
    return pmcf != NULL && pmcf->backend != NGX_HTTP_CACHE_TAG_BACKEND_NONE;
}

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_open_writer(ngx_http_cache_purge_main_conf_t *pmcf,
                                     ngx_log_t *log) {
    if (pmcf == NULL) {
        return NULL;
    }

    switch (pmcf->backend) {
#if (NGX_CACHE_PURGE_SQLITE)
    case NGX_HTTP_CACHE_TAG_BACKEND_SQLITE:
        return ngx_http_cache_tag_store_sqlite_open(pmcf, 0, log);
#endif
    case NGX_HTTP_CACHE_TAG_BACKEND_REDIS:
        return ngx_http_cache_tag_store_redis_open(pmcf, 0, log);
    default:
        return NULL;
    }
}

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_open_reader(ngx_http_cache_purge_main_conf_t *pmcf,
                                     ngx_log_t *log) {
    if (pmcf == NULL) {
        return NULL;
    }

    switch (pmcf->backend) {
#if (NGX_CACHE_PURGE_SQLITE)
    case NGX_HTTP_CACHE_TAG_BACKEND_SQLITE:
        return ngx_http_cache_tag_store_sqlite_open(pmcf, 1, log);
#endif
    case NGX_HTTP_CACHE_TAG_BACKEND_REDIS:
        return ngx_http_cache_tag_store_redis_open(pmcf, 1, log);
    default:
        return NULL;
    }
}

void
ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_t *store) {
    if (store == NULL) {
        return;
    }

    /* Close the backing resource (socket / db handle).  The store struct
     * itself is allocated from cycle->pool and freed automatically when
     * the pool is destroyed on worker exit or config reload. */
    store->ops->close(store);
}

ngx_int_t
ngx_http_cache_tag_store_begin_batch(ngx_http_cache_tag_store_t *store,
                                     ngx_log_t *log) {
    return store->ops->begin_batch(store, log);
}

ngx_int_t
ngx_http_cache_tag_store_commit_batch(ngx_http_cache_tag_store_t *store,
                                      ngx_log_t *log) {
    return store->ops->commit_batch(store, log);
}

ngx_int_t
ngx_http_cache_tag_store_rollback_batch(ngx_http_cache_tag_store_t *store,
                                        ngx_log_t *log) {
    return store->ops->rollback_batch(store, log);
}

ngx_int_t
ngx_http_cache_tag_store_replace_file_tags(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, time_t mtime, off_t size,
        ngx_array_t *tags, ngx_log_t *log) {
    return store->ops->replace_file_tags(store, zone_name, path, mtime, size,
                                         tags, log);
}

ngx_int_t
ngx_http_cache_tag_store_delete_file(ngx_http_cache_tag_store_t *store,
                                     ngx_str_t *zone_name, ngx_str_t *path,
                                     ngx_log_t *log) {
    return store->ops->delete_file(store, zone_name, path, log);
}

ngx_int_t
ngx_http_cache_tag_store_collect_paths_by_tags(ngx_http_cache_tag_store_t *store,
        ngx_pool_t *pool, ngx_str_t *zone_name, ngx_array_t *tags,
        ngx_array_t **paths, ngx_log_t *log) {
    return store->ops->collect_paths_by_tags(store, pool, zone_name, tags, paths,
            log);
}

ngx_int_t
ngx_http_cache_tag_store_get_zone_state(ngx_http_cache_tag_store_t *store,
                                        ngx_str_t *zone_name,
                                        ngx_http_cache_tag_zone_state_t *state,
                                        ngx_log_t *log) {
    return store->ops->get_zone_state(store, zone_name, state, log);
}

ngx_int_t
ngx_http_cache_tag_store_set_zone_state(ngx_http_cache_tag_store_t *store,
                                        ngx_str_t *zone_name,
                                        ngx_http_cache_tag_zone_state_t *state,
                                        ngx_log_t *log) {
    return store->ops->set_zone_state(store, zone_name, state, log);
}

ngx_int_t
ngx_http_cache_tag_store_process_file(ngx_http_cache_tag_store_t *store,
                                      ngx_str_t *zone_name, ngx_str_t *path,
                                      ngx_array_t *headers,
                                      ngx_log_t *log) {
    ngx_pool_t   *pool;
    ngx_array_t  *tags;
    time_t        mtime;
    off_t         size;
    ngx_int_t     rc;

    pool = ngx_create_pool(4096, log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    if (headers == NULL || headers->nelts == 0) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_tag_store_delete_file(store, zone_name, path, log);
    }

    if (ngx_http_cache_tag_parse_file(pool, path, headers, &tags, &mtime, &size,
                                      log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_tag_store_delete_file(store, zone_name, path, log);
    }

    rc = ngx_http_cache_tag_store_replace_file_tags(store, zone_name, path,
            mtime, size, tags, log);
    ngx_destroy_pool(pool);

    return rc;
}

ngx_int_t
ngx_http_cache_tag_store_runtime_init(ngx_cycle_t *cycle,
                                      ngx_http_cache_purge_main_conf_t *pmcf,
                                      ngx_flag_t owner) {
    ngx_memzero(&ngx_http_cache_tag_store_runtime,
                sizeof(ngx_http_cache_tag_store_runtime));

    ngx_http_cache_tag_store_runtime.cycle = cycle;
    ngx_http_cache_tag_store_runtime.owner = owner;

    if (!ngx_http_cache_tag_store_configured(pmcf) || !owner) {
        return NGX_OK;
    }

    ngx_http_cache_tag_store_runtime.writer =
        ngx_http_cache_tag_store_open_writer(pmcf, cycle->log);

    return ngx_http_cache_tag_store_runtime.writer != NULL ? NGX_OK : NGX_ERROR;
}

void
ngx_http_cache_tag_store_runtime_shutdown(void) {
    ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_runtime.writer);
    ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_runtime.reader);

    ngx_memzero(&ngx_http_cache_tag_store_runtime,
                sizeof(ngx_http_cache_tag_store_runtime));
}

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_writer(void) {
    return ngx_http_cache_tag_store_runtime.writer;
}

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_reader(ngx_http_cache_purge_main_conf_t *pmcf,
                                ngx_log_t *log) {
    if (ngx_http_cache_tag_store_runtime.reader != NULL) {
        return ngx_http_cache_tag_store_runtime.reader;
    }

    ngx_http_cache_tag_store_runtime.reader =
        ngx_http_cache_tag_store_open_reader(pmcf, log);

    return ngx_http_cache_tag_store_runtime.reader;
}

ngx_int_t
ngx_http_cache_tag_extract_tokens(ngx_pool_t *pool, u_char *value, size_t len,
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

            if (ngx_http_cache_tag_push_unique(pool, tags, value + start,
                                               end - start) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_push_unique(ngx_pool_t *pool, ngx_array_t *tags,
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
ngx_http_cache_tag_path_known(ngx_str_t *path) {
    u_char  *p;

    p = path->data + path->len;
    while (p > path->data && p[-1] != '/') {
        p--;
    }

    return (size_t)(path->data + path->len - p) == 2 * NGX_HTTP_CACHE_KEY_LEN;
}

static ngx_int_t
ngx_http_cache_tag_parse_file(ngx_pool_t *pool, ngx_str_t *path,
                              ngx_array_t *headers, ngx_array_t **tags,
                              time_t *mtime, off_t *size, ngx_log_t *log) {
    ngx_file_info_t  fi;
    ngx_file_t       file;
    u_char          *buf;
    ssize_t          n;
    size_t           max_read, i, j, line_end, value_start;
    ngx_array_t     *result;
    ngx_str_t       *header;

    if (!ngx_http_cache_tag_path_known(path)) {
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

    header = headers->elts;
    for (i = 0; i < (size_t) n; i++) {
        if (i != 0 && buf[i - 1] != '\n') {
            continue;
        }

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

            if (ngx_http_cache_tag_extract_tokens(pool, buf + value_start,
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
