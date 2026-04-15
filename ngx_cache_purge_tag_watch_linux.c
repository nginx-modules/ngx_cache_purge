#include "ngx_cache_purge_tag.h"

#if (NGX_LINUX)

static ngx_http_cache_tag_runtime_t ngx_http_cache_tag_runtime;

static void ngx_http_cache_tag_timer_handler(ngx_event_t *ev);
static ngx_http_cache_tag_watch_t *ngx_http_cache_tag_find_watch(int wd);
static ngx_int_t ngx_http_cache_tag_remove_watch(int wd, ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_add_watch(sqlite3 *db,
        ngx_http_cache_tag_zone_t *zone, ngx_str_t *path, ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_add_watch_recursive(sqlite3 *db,
        ngx_http_cache_tag_zone_t *zone, ngx_str_t *path, ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_process_events(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_join_path(ngx_pool_t *pool, ngx_str_t *base,
        const char *name, ngx_str_t *out);

ngx_http_cache_tag_zone_t *
ngx_http_cache_tag_lookup_zone(ngx_http_file_cache_t *cache) {
    ngx_http_conf_ctx_t              *http_ctx;
    ngx_http_cache_purge_main_conf_t *pmcf;
    ngx_http_cache_tag_zone_t        *zone;
    ngx_uint_t                        i;

    http_ctx = (ngx_http_conf_ctx_t *) ngx_get_conf(ngx_cycle->conf_ctx,
               ngx_http_module);
    pmcf = http_ctx->main_conf[ngx_http_cache_purge_module.ctx_index];
    if (pmcf == NULL || pmcf->zones == NULL) {
        return NULL;
    }

    zone = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        if (zone[i].cache == cache) {
            return &zone[i];
        }
    }

    return NULL;
}

ngx_int_t
ngx_http_cache_tag_bootstrap_zone(sqlite3 *db, ngx_http_cache_tag_zone_t *zone,
                                  ngx_cycle_t *cycle) {
    if (zone == NULL || zone->cache == NULL || zone->cache->path == NULL) {
        return NGX_DECLINED;
    }

    return ngx_http_cache_tag_add_watch_recursive(db, zone,
            &zone->cache->path->name,
            cycle);
}

static ngx_http_cache_tag_watch_t *
ngx_http_cache_tag_find_watch(int wd) {
    ngx_http_cache_tag_watch_t  *watch;
    ngx_uint_t                   i;

    if (ngx_http_cache_tag_runtime.watches == NULL) {
        return NULL;
    }

    watch = ngx_http_cache_tag_runtime.watches->elts;
    for (i = 0; i < ngx_http_cache_tag_runtime.watches->nelts; i++) {
        if (watch[i].wd == wd) {
            return &watch[i];
        }
    }

    return NULL;
}

static ngx_int_t
ngx_http_cache_tag_remove_watch(int wd, ngx_cycle_t *cycle) {
    ngx_http_cache_tag_watch_t  *watch;
    ngx_uint_t                   i;

    (void) cycle;

    if (ngx_http_cache_tag_runtime.watches == NULL) {
        return NGX_OK;
    }

    watch = ngx_http_cache_tag_runtime.watches->elts;
    for (i = 0; i < ngx_http_cache_tag_runtime.watches->nelts; i++) {
        if (watch[i].wd == wd) {
            inotify_rm_watch(ngx_http_cache_tag_runtime.inotify_fd, wd);
            if (i + 1 < ngx_http_cache_tag_runtime.watches->nelts) {
                watch[i] = watch[ngx_http_cache_tag_runtime.watches->nelts - 1];
            }
            ngx_http_cache_tag_runtime.watches->nelts--;
            return NGX_OK;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_add_watch(sqlite3 *db, ngx_http_cache_tag_zone_t *zone,
                             ngx_str_t *path, ngx_cycle_t *cycle) {
    ngx_http_cache_tag_watch_t  *watch;
    int                          wd;

    (void) db;

    if (ngx_http_cache_tag_runtime.watches == NULL) {
        ngx_http_cache_tag_runtime.watches = ngx_array_create(
                cycle->pool, 16, sizeof(ngx_http_cache_tag_watch_t));
        if (ngx_http_cache_tag_runtime.watches == NULL) {
            return NGX_ERROR;
        }
    }

    if (ngx_http_cache_tag_runtime.inotify_fd <= 0) {
        return NGX_DECLINED;
    }

    wd = inotify_add_watch(ngx_http_cache_tag_runtime.inotify_fd,
                           (const char *) path->data,
                           IN_CREATE|IN_MOVED_TO|IN_CLOSE_WRITE|IN_DELETE
                           |IN_MOVED_FROM|IN_DELETE_SELF|IN_ONLYDIR);
    if (wd == -1) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "inotify_add_watch failed for \"%V\"", path);
        return NGX_DECLINED;
    }

    watch = ngx_http_cache_tag_find_watch(wd);
    if (watch != NULL) {
        return NGX_OK;
    }

    watch = ngx_array_push(ngx_http_cache_tag_runtime.watches);
    if (watch == NULL) {
        return NGX_ERROR;
    }

    watch->wd = wd;
    watch->cache = zone->cache;
    watch->zone_name = zone->zone_name;
    watch->path.len = path->len;
    watch->path.data = ngx_pnalloc(cycle->pool, path->len + 1);
    if (watch->path.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(watch->path.data, path->data, path->len);
    watch->path.data[path->len] = '\0';

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_add_watch_recursive(sqlite3 *db, ngx_http_cache_tag_zone_t *zone,
                                       ngx_str_t *path, ngx_cycle_t *cycle) {
    DIR            *dir;
    struct dirent  *entry;
    ngx_str_t       child;

    if (ngx_http_cache_tag_add_watch(db, zone, path, cycle) == NGX_ERROR) {
        return NGX_ERROR;
    }

    dir = opendir((const char *) path->data);
    if (dir == NULL) {
        return NGX_DECLINED;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (ngx_strcmp(entry->d_name, ".") == 0
                || ngx_strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (ngx_http_cache_tag_join_path(cycle->pool, path, entry->d_name, &child)
                != NGX_OK) {
            closedir(dir);
            return NGX_ERROR;
        }

        if (entry->d_type == DT_DIR) {
            if (ngx_http_cache_tag_add_watch_recursive(db, zone, &child, cycle)
                    != NGX_OK) {
                continue;
            }
            continue;
        }

        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            if (ngx_http_cache_tag_process_file(db, &zone->zone_name, &child,
                                                cycle->log)
                    != NGX_OK) {
                continue;
            }
        }
    }

    closedir(dir);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_process_events(ngx_cycle_t *cycle) {
    u_char                      buf[8192];
    ssize_t                     n;
    size_t                      offset;
    struct inotify_event       *event;
    ngx_http_cache_tag_watch_t *watch;
    ngx_str_t                   path;
    ngx_pool_t                 *pool;

    for (;;) {
        n = read(ngx_http_cache_tag_runtime.inotify_fd, buf, sizeof(buf));
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return NGX_OK;
            }
            return NGX_ERROR;
        }

        if (n == 0) {
            return NGX_OK;
        }

        pool = ngx_create_pool(4096, cycle->log);
        if (pool == NULL) {
            return NGX_ERROR;
        }

        offset = 0;
        while (offset < (size_t) n) {
            event = (struct inotify_event *)(buf + offset);
            watch = ngx_http_cache_tag_find_watch(event->wd);
            if (watch != NULL) {
                if (event->mask & (IN_DELETE_SELF|IN_IGNORED)) {
                    ngx_http_cache_tag_remove_watch(event->wd, cycle);
                } else if (event->len > 0
                           && ngx_http_cache_tag_join_path(pool, &watch->path,
                                   event->name, &path) == NGX_OK) {
                    if (event->mask & IN_ISDIR) {
                        if (event->mask & (IN_CREATE|IN_MOVED_TO)) {
                            ngx_http_cache_tag_zone_t zone;
                            zone.zone_name = watch->zone_name;
                            zone.cache = watch->cache;
                            ngx_http_cache_tag_add_watch_recursive(
                                ngx_http_cache_tag_runtime.db, &zone, &path, cycle);
                        }
                    } else if (event->mask & (IN_CREATE|IN_MOVED_TO|IN_CLOSE_WRITE)) {
                        ngx_http_cache_tag_process_file(ngx_http_cache_tag_runtime.db,
                                                        &watch->zone_name, &path,
                                                        cycle->log);
                    } else if (event->mask & (IN_DELETE|IN_MOVED_FROM)) {
                        ngx_http_cache_tag_db_delete_file(ngx_http_cache_tag_runtime.db,
                                                          &watch->zone_name, &path,
                                                          cycle->log);
                    }
                }
            }

            offset += sizeof(struct inotify_event) + event->len;
        }

        ngx_destroy_pool(pool);
    }
}

static void
ngx_http_cache_tag_timer_handler(ngx_event_t *ev) {
    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    if (ngx_http_cache_tag_process_events(ngx_http_cache_tag_runtime.cycle) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ngx_http_cache_tag_runtime.cycle->log, 0,
                      "cache_tag watcher processing failed");
    }

    ngx_add_timer(ev, 250);
}

ngx_int_t
ngx_http_cache_tag_init_runtime(ngx_cycle_t *cycle,
                                ngx_http_cache_purge_main_conf_t *pmcf) {
    ngx_http_cache_tag_zone_t  *zone;
    ngx_uint_t                  i;

    ngx_memzero(&ngx_http_cache_tag_runtime, sizeof(ngx_http_cache_tag_runtime));

    ngx_http_cache_tag_runtime.owner = (ngx_process == NGX_PROCESS_WORKER && ngx_worker == 0);
    if (!ngx_http_cache_tag_runtime.owner || pmcf->zones->nelts == 0) {
        return NGX_OK;
    }

    ngx_http_cache_tag_runtime.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ngx_http_cache_tag_runtime.inotify_fd == -1) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, ngx_errno,
                      "inotify_init1 failed");
        return NGX_ERROR;
    }

    ngx_http_cache_tag_runtime.db = ngx_http_cache_tag_db_open(
                                        &pmcf->sqlite_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, cycle->log);
    if (ngx_http_cache_tag_runtime.db == NULL) {
        close(ngx_http_cache_tag_runtime.inotify_fd);
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_sqlite_init(ngx_http_cache_tag_runtime.db, cycle->log)
            != NGX_OK) {
        ngx_http_cache_tag_shutdown_runtime();
        return NGX_ERROR;
    }

    ngx_http_cache_tag_runtime.watches = ngx_array_create(cycle->pool, 16,
                                         sizeof(ngx_http_cache_tag_watch_t));
    if (ngx_http_cache_tag_runtime.watches == NULL) {
        ngx_http_cache_tag_shutdown_runtime();
        return NGX_ERROR;
    }

    zone = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        if (zone[i].cache == NULL || zone[i].cache->path == NULL) {
            continue;
        }

        if (ngx_http_cache_tag_add_watch_recursive(ngx_http_cache_tag_runtime.db,
                &zone[i], &zone[i].cache->path->name,
                cycle) == NGX_ERROR) {
            ngx_http_cache_tag_shutdown_runtime();
            return NGX_ERROR;
        }
    }

    ngx_http_cache_tag_runtime.cycle = cycle;
    ngx_http_cache_tag_runtime.timer.handler = ngx_http_cache_tag_timer_handler;
    ngx_http_cache_tag_runtime.timer.log = cycle->log;
    ngx_http_cache_tag_runtime.timer.data = NULL;
    ngx_http_cache_tag_runtime.timer.cancelable = 1;
    ngx_add_timer(&ngx_http_cache_tag_runtime.timer, 250);

    ngx_http_cache_tag_runtime.initialized = 1;
    ngx_http_cache_tag_runtime.active = 1;

    return NGX_OK;
}

void
ngx_http_cache_tag_shutdown_runtime(void) {
    if (ngx_http_cache_tag_runtime.timer.timer_set) {
        ngx_del_timer(&ngx_http_cache_tag_runtime.timer);
    }

    if (ngx_http_cache_tag_runtime.db != NULL) {
        sqlite3_close(ngx_http_cache_tag_runtime.db);
    }

    if (ngx_http_cache_tag_runtime.inotify_fd > 0) {
        close(ngx_http_cache_tag_runtime.inotify_fd);
    }

    ngx_memzero(&ngx_http_cache_tag_runtime, sizeof(ngx_http_cache_tag_runtime));
}

static ngx_int_t
ngx_http_cache_tag_join_path(ngx_pool_t *pool, ngx_str_t *base, const char *name,
                             ngx_str_t *out) {
    size_t  name_len;

    name_len = ngx_strlen(name);
    out->len = base->len + 1 + name_len;
    out->data = ngx_pnalloc(pool, out->len + 1);
    if (out->data == NULL) {
        return NGX_ERROR;
    }

    ngx_snprintf(out->data, out->len + 1, "%V/%s%Z", base, name);

    return NGX_OK;
}

#endif
