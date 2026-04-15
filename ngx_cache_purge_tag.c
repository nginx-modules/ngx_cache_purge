#include "ngx_cache_purge_tag.h"

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
ngx_http_cache_tag_register_cache(ngx_conf_t *cf, ngx_http_file_cache_t *cache) {
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
            return NGX_OK;
        }
    }

    zone = ngx_array_push(pmcf->zones);
    if (zone == NULL) {
        return NGX_ERROR;
    }

    zone->cache = cache;
    zone->zone_name = cache->shm_zone->shm.name;

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_purge(ngx_http_request_t *r, ngx_http_file_cache_t *cache) {
    ngx_http_conf_ctx_t              *http_ctx;
    ngx_http_cache_purge_main_conf_t *pmcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_tag_zone_t        *zone;
    ngx_array_t                      *tags, *paths;
    ngx_str_t                        *path;
    ngx_uint_t                        i;
    ngx_int_t                         rc, purged;
#if (NGX_LINUX)
    sqlite3                          *db;
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
    }

#if !(NGX_LINUX)
    return NGX_DECLINED;
#else
    db = ngx_http_cache_tag_db_open(&pmcf->sqlite_path,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                    r->connection->log);
    if (db == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_sqlite_init(db, r->connection->log) != NGX_OK) {
        sqlite3_close(db);
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_db_collect_paths(db, r->pool, &zone->zone_name,
                                            tags, &paths, r->connection->log)
            != NGX_OK) {
        sqlite3_close(db);
        return NGX_ERROR;
    }

    if (paths->nelts == 0 && cache->path != NULL) {
        ngx_http_cache_tag_bootstrap_zone(db, zone, (ngx_cycle_t *) ngx_cycle);
        if (ngx_http_cache_tag_db_collect_paths(db, r->pool, &zone->zone_name,
                                                tags, &paths, r->connection->log)
                != NGX_OK) {
            sqlite3_close(db);
            return NGX_ERROR;
        }
    }

    purged = 0;
    path = paths->elts;
    for (i = 0; i < paths->nelts; i++) {
        rc = ngx_http_cache_purge_by_path(cache, &path[i], cplcf->conf->soft,
                                          r->connection->log);
        if (rc == NGX_OK) {
            purged++;
            ngx_http_cache_tag_db_delete_file(db, &zone->zone_name, &path[i],
                                              r->connection->log);
        } else if (rc == NGX_DECLINED) {
            ngx_http_cache_tag_db_delete_file(db, &zone->zone_name, &path[i],
                                              r->connection->log);
        } else {
            sqlite3_close(db);
            return NGX_ERROR;
        }
    }

    sqlite3_close(db);

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
