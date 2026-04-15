#include "ngx_cache_purge_tag.h"

static ngx_flag_t ngx_http_cache_tag_headers_equal(ngx_array_t *left,
        ngx_array_t *right);

char *
ngx_http_cache_tag_index_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_main_conf_t  *pmcf;
    ngx_str_t                         *value;

    pmcf = conf;
    value = cf->args->elts;

    if (pmcf->sqlite_path.data != NULL) {
        return "is duplicate";
    }

    if (ngx_strcmp(value[1].data, "sqlite") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid cache_tag_index backend \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

#if !(NGX_LINUX)
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "cache_tag_index requires Linux inotify support");
    return NGX_CONF_ERROR;
#else
    pmcf->sqlite_path = value[2];
    return NGX_CONF_OK;
#endif
}

char *
ngx_http_cache_tag_headers_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_str_t                        *value, *header;
    ngx_uint_t                        i;

    cplcf = conf;

    if (cplcf->cache_tag_headers != NULL && cf->cmd_type == NGX_HTTP_LOC_CONF) {
        return "is duplicate";
    }

    cplcf->cache_tag_headers = ngx_array_create(cf->pool, cf->args->nelts - 1,
                               sizeof(ngx_str_t));
    if (cplcf->cache_tag_headers == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        header = ngx_array_push(cplcf->cache_tag_headers);
        if (header == NULL) {
            return NGX_CONF_ERROR;
        }

        *header = value[i];
    }

    return NGX_CONF_OK;
}

char *
ngx_http_cache_tag_watch_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_str_t                        *value;

    cplcf = conf;
    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "on") == 0) {
        cplcf->cache_tag_watch = 1;
        return NGX_CONF_OK;
    }

    if (ngx_strcmp(value[1].data, "off") == 0) {
        cplcf->cache_tag_watch = 0;
        return NGX_CONF_OK;
    }

    return "invalid value";
}

ngx_flag_t
ngx_http_cache_tag_location_enabled(ngx_http_cache_purge_loc_conf_t *cplcf) {
    return cplcf->cache_tag_watch && cplcf->cache_tag_headers != NULL;
}

ngx_int_t
ngx_http_cache_tag_request_headers(ngx_http_request_t *r, ngx_array_t **tags) {
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_list_part_t                  *part;
    ngx_table_elt_t                  *header;
    ngx_str_t                        *wanted;
    ngx_uint_t                        i, j;
    ngx_array_t                      *result;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    if (!ngx_http_cache_tag_location_enabled(cplcf)) {
        *tags = NULL;
        return NGX_DECLINED;
    }

    result = ngx_array_create(r->pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    part = &r->headers_in.headers.part;
    header = part->elts;
    wanted = cplcf->cache_tag_headers->elts;

    for (i = 0; ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        for (j = 0; j < cplcf->cache_tag_headers->nelts; j++) {
            if (header[i].key.len != wanted[j].len) {
                continue;
            }

            if (ngx_strncasecmp(header[i].key.data, wanted[j].data,
                                wanted[j].len) != 0) {
                continue;
            }

            if (ngx_http_cache_tag_extract_tokens(r->pool, header[i].value.data,
                                                  header[i].value.len, result)
                    != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    *tags = result;

    return result->nelts > 0 ? NGX_OK : NGX_DECLINED;
}

ngx_int_t
ngx_http_cache_tag_register_cache(ngx_conf_t *cf, ngx_http_file_cache_t *cache,
                                  ngx_array_t *headers) {
    ngx_http_cache_purge_main_conf_t  *pmcf;
    ngx_http_cache_tag_zone_t         *zones, *zone;
    ngx_uint_t                         i;

    if (cache == NULL) {
        return NGX_OK;
    }

    pmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);
    if (pmcf == NULL || pmcf->sqlite_path.len == 0) {
        return NGX_OK;
    }

    zones = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        if (zones[i].cache == cache) {
            if (!ngx_http_cache_tag_headers_equal(zones[i].headers, headers)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "cache_tag_headers must match for all watched locations using cache zone \"%V\"",
                                   &zones[i].zone_name);
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }

    zone = ngx_array_push(pmcf->zones);
    if (zone == NULL) {
        return NGX_ERROR;
    }

    zone->cache = cache;
    zone->zone_name = cache->shm_zone->shm.name;
    zone->headers = headers;

    return NGX_OK;
}

static ngx_flag_t
ngx_http_cache_tag_headers_equal(ngx_array_t *left, ngx_array_t *right) {
    ngx_str_t   *left_header, *right_header;
    ngx_uint_t   i, j;

    if (left == right) {
        return 1;
    }

    if (left == NULL || right == NULL || left->nelts != right->nelts) {
        return 0;
    }

    left_header = left->elts;
    right_header = right->elts;

    for (i = 0; i < left->nelts; i++) {
        for (j = 0; j < right->nelts; j++) {
            if (left_header[i].len == right_header[j].len
                    && ngx_strncasecmp(left_header[i].data,
                                       right_header[j].data,
                                       left_header[i].len) == 0) {
                break;
            }
        }

        if (j == right->nelts) {
            return 0;
        }
    }

    return 1;
}

ngx_int_t
ngx_http_cache_tag_purge(ngx_http_request_t *r, ngx_http_file_cache_t *cache) {
    ngx_http_conf_ctx_t              *http_ctx;
    ngx_http_cache_purge_main_conf_t *pmcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_tag_zone_t        *zone;
    ngx_http_cache_tag_zone_state_t   state;
    ngx_array_t                      *tags, *paths;
    ngx_str_t                        *path;
    ngx_uint_t                        i;
    ngx_int_t                         rc, purged;
#if (NGX_LINUX)
    ngx_http_cache_tag_store_t       *reader, *writer;
#endif

    rc = ngx_http_cache_tag_request_headers(r, &tags);
    if (rc != NGX_OK) {
        return NGX_DECLINED;
    }

    http_ctx = (ngx_http_conf_ctx_t *) ngx_get_conf(ngx_cycle->conf_ctx,
               ngx_http_module);
    pmcf = http_ctx->main_conf[ngx_http_cache_purge_module.ctx_index];
    if (pmcf == NULL || pmcf->sqlite_path.len == 0) {
        return NGX_DECLINED;
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    zone = NULL;
#if (NGX_LINUX)
    zone = ngx_http_cache_tag_lookup_zone(cache);
#endif
    if (zone == NULL) {
        zone = ngx_pcalloc(r->pool, sizeof(ngx_http_cache_tag_zone_t));
        if (zone == NULL) {
            return NGX_ERROR;
        }
        zone->cache = cache;
        zone->zone_name = cache->shm_zone->shm.name;
        zone->headers = cplcf->cache_tag_headers;
    }

#if !(NGX_LINUX)
    return NGX_DECLINED;
#else
    reader = ngx_http_cache_tag_store_reader(pmcf, r->connection->log);
    if (reader == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_flush_pending((ngx_cycle_t *) ngx_cycle) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_get_zone_state(reader, &zone->zone_name, &state,
            r->connection->log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (state.bootstrap_complete) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "cache_tag request reusing persisted index for zone \"%V\"",
                      &zone->zone_name);
    }

    if (ngx_http_cache_tag_store_collect_paths_by_tags(reader, r->pool,
            &zone->zone_name, tags, &paths, r->connection->log)
            != NGX_OK) {
        return NGX_ERROR;
    }

    if (paths->nelts == 0 && !state.bootstrap_complete && cache->path != NULL) {
        writer = ngx_http_cache_tag_store_open_writer(&pmcf->sqlite_path,
                 r->connection->log);
        if (writer == NULL) {
            return NGX_ERROR;
        }

        rc = ngx_http_cache_tag_bootstrap_zone(writer, zone,
                                               (ngx_cycle_t *) ngx_cycle);
        ngx_http_cache_tag_store_close(writer);
        if (rc != NGX_OK) {
            return NGX_ERROR;
        }

        if (ngx_http_cache_tag_store_get_zone_state(reader, &zone->zone_name,
                &state, r->connection->log) != NGX_OK) {
            return NGX_ERROR;
        }

        if (ngx_http_cache_tag_store_collect_paths_by_tags(reader, r->pool,
                &zone->zone_name, tags, &paths, r->connection->log)
                != NGX_OK) {
            return NGX_ERROR;
        }
    }

    writer = NULL;
    purged = 0;
    path = paths->elts;
    for (i = 0; i < paths->nelts; i++) {
        rc = ngx_http_cache_purge_by_path(cache, &path[i], cplcf->conf->soft,
                                          r->connection->log);
        if (rc == NGX_OK) {
            purged++;
        } else if (rc != NGX_DECLINED) {
            ngx_http_cache_tag_store_close(writer);
            return NGX_ERROR;
        }

        if (rc == NGX_OK && cplcf->conf->soft) {
            continue;
        }

        if (rc == NGX_OK || rc == NGX_DECLINED) {
            if (writer == NULL) {
                writer = ngx_http_cache_tag_store_open_writer(&pmcf->sqlite_path,
                         r->connection->log);
                if (writer == NULL) {
                    return NGX_ERROR;
                }
            }

            if (ngx_http_cache_tag_store_delete_file(writer, &zone->zone_name,
                    &path[i], r->connection->log) != NGX_OK) {
                ngx_http_cache_tag_store_close(writer);
                return NGX_ERROR;
            }
        }
    }

    ngx_http_cache_tag_store_close(writer);

    return purged > 0 ? NGX_OK : NGX_DECLINED;
#endif
}

ngx_int_t
ngx_http_cache_tag_process_init(ngx_cycle_t *cycle,
                                ngx_http_cache_purge_main_conf_t *pmcf) {
#if !(NGX_LINUX)
    (void) cycle;
    (void) pmcf;
    return NGX_OK;
#else
    return ngx_http_cache_tag_init_runtime(cycle, pmcf);
#endif
}

void
ngx_http_cache_tag_process_exit(void) {
#if (NGX_LINUX)
    ngx_http_cache_tag_shutdown_runtime();
#endif
}
