#include "ngx_cache_purge_tag.h"

#if (NGX_LINUX)

static ngx_http_cache_tag_watch_runtime_t ngx_http_cache_tag_watch_runtime;
static ngx_http_cache_tag_queue_ctx_t    *ngx_http_cache_tag_queue_ctx;

static void ngx_http_cache_tag_inotify_handler(ngx_event_t *ev);
static void ngx_http_cache_tag_timer_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_cache_tag_read_inotify(ngx_pool_t *pool,
        ngx_array_t *pending_ops, ngx_cycle_t *cycle);
static void ngx_http_cache_tag_queue_log_stats(ngx_log_t *log, ngx_uint_t level,
        const char *message);
static ngx_int_t ngx_http_cache_tag_queue_init_zone(ngx_shm_zone_t *shm_zone,
        void *data);
static void ngx_http_cache_tag_zone_insert(ngx_rbtree_node_t *temp,
        ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static void ngx_http_cache_tag_watch_insert(ngx_rbtree_node_t *temp,
        ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_http_cache_tag_zone_index_t *ngx_http_cache_tag_lookup_zone_index(
    ngx_http_file_cache_t *cache);
static ngx_http_cache_tag_watch_t *ngx_http_cache_tag_find_watch(int wd);
static ngx_int_t ngx_http_cache_tag_remove_watch(int wd);
static ngx_int_t ngx_http_cache_tag_add_watch(ngx_http_cache_tag_zone_t *zone,
        ngx_str_t *path, ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_add_watch_recursive(
    ngx_http_cache_tag_store_t *store, ngx_http_cache_tag_zone_t *zone,
    ngx_str_t *path, ngx_cycle_t *cycle, ngx_uint_t index_mode,
    ngx_array_t *pending_ops, time_t min_mtime);
static ngx_int_t ngx_http_cache_tag_process_events(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_join_path(ngx_pool_t *pool, ngx_str_t *base,
        const char *name, ngx_str_t *out);
static ngx_int_t ngx_http_cache_tag_runtime_init_zones(
    ngx_cycle_t *cycle, ngx_http_cache_purge_main_conf_t *pmcf);
static ngx_int_t ngx_http_cache_tag_pending_op_set(ngx_pool_t *pool,
        ngx_array_t *pending_ops, ngx_str_t *zone_name, ngx_http_file_cache_t *cache,
        ngx_str_t *path, ngx_uint_t operation);
static ngx_int_t ngx_http_cache_tag_queue_drain(ngx_pool_t *pool,
        ngx_array_t *pending_ops, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_apply_pending_ops(
    ngx_http_cache_tag_store_t *store, ngx_array_t *pending_ops, ngx_log_t *log);

#define NGX_HTTP_CACHE_TAG_INDEX_NONE    0
#define NGX_HTTP_CACHE_TAG_INDEX_DIRECT  1
#define NGX_HTTP_CACHE_TAG_INDEX_PENDING 2

ngx_int_t
ngx_http_cache_tag_queue_init_conf(ngx_conf_t *cf,
                                   ngx_http_cache_purge_main_conf_t *pmcf) {
    static ngx_str_t  queue_name = ngx_string("ngx_cache_purge_tag_queue");
    ngx_http_cache_tag_queue_ctx_t  *ctx;

    if (pmcf->queue_zone != NULL) {
        return NGX_OK;
    }

    pmcf->queue_zone = ngx_shared_memory_add(cf, &queue_name,
                       pmcf->queue_shm_size,
                       &ngx_http_cache_purge_module);
    if (pmcf->queue_zone == NULL) {
        return NGX_ERROR;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_tag_queue_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    pmcf->queue_zone->init = ngx_http_cache_tag_queue_init_zone;
    pmcf->queue_zone->data = ctx;

    return NGX_OK;
}

ngx_flag_t
ngx_http_cache_tag_is_owner(void) {
    return ngx_http_cache_tag_watch_runtime.owner;
}

ngx_int_t
ngx_http_cache_tag_queue_enqueue_delete(ngx_http_cache_purge_main_conf_t *pmcf,
                                        ngx_str_t *zone_name, ngx_str_t *path,
                                        ngx_log_t *log) {
    ngx_http_cache_tag_queue_ctx_t     *ctx;
    ngx_http_cache_tag_queue_shctx_t   *sh;
    ngx_http_cache_tag_queue_entry_t   *entry;

    if (pmcf == NULL || pmcf->queue_zone == NULL || pmcf->queue_zone->data == NULL) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "cache_tag queue unavailable for zone \"%V\" path \"%V\"",
                      zone_name, path);
        return NGX_DECLINED;
    }

    ctx = pmcf->queue_zone->data;

    if (zone_name->len >= NGX_HTTP_CACHE_TAG_QUEUE_ZONE_NAME_MAX
            || path->len >= NGX_HTTP_CACHE_TAG_QUEUE_PATH_MAX) {
        ctx->stats.rejected++;
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "cache_tag queue entry too large for zone \"%V\" path \"%V\"",
                      zone_name, path);
        ngx_http_cache_tag_queue_log_stats(log, NGX_LOG_WARN,
                                           "cache_tag queue rejected delete");
        return NGX_DECLINED;
    }

    if (ctx->shpool == NULL || ctx->sh == NULL) {
        ctx->stats.rejected++;
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "cache_tag queue not initialized for zone \"%V\" path \"%V\"",
                      zone_name, path);
        ngx_http_cache_tag_queue_log_stats(log, NGX_LOG_WARN,
                                           "cache_tag queue rejected delete");
        return NGX_DECLINED;
    }

    sh = ctx->sh;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    if (sh->count >= sh->capacity) {
        sh->dropped++;
        ctx->stats.rejected++;
        ngx_shmtx_unlock(&ctx->shpool->mutex);

        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "cache_tag queue full, dropping delete for zone \"%V\" path \"%V\"",
                      zone_name, path);
        ngx_http_cache_tag_queue_log_stats(log, NGX_LOG_WARN,
                                           "cache_tag queue rejected delete");
        return NGX_DECLINED;
    }

    entry = &sh->entries[sh->tail];
    entry->operation = NGX_HTTP_CACHE_TAG_OP_DELETE;
    entry->zone_name_len = zone_name->len;
    entry->path_len = path->len;
    ngx_memcpy(entry->zone_name, zone_name->data, zone_name->len);
    entry->zone_name[zone_name->len] = '\0';
    ngx_memcpy(entry->path, path->data, path->len);
    entry->path[path->len] = '\0';

    sh->tail = (sh->tail + 1) % sh->capacity;
    sh->count++;
    ctx->stats.accepted++;

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}

ngx_http_cache_tag_zone_t *
ngx_http_cache_tag_lookup_zone(ngx_http_file_cache_t *cache) {
    ngx_http_cache_tag_zone_index_t  *index;

    index = ngx_http_cache_tag_lookup_zone_index(cache);
    if (index == NULL) {
        return NULL;
    }

    return index->zone;
}

ngx_int_t
ngx_http_cache_tag_bootstrap_zone(ngx_http_cache_tag_store_t *store,
                                  ngx_http_cache_tag_zone_t *zone,
                                  ngx_cycle_t *cycle) {
    ngx_http_cache_tag_zone_state_t  state;

    if (zone == NULL || zone->cache == NULL || zone->cache->path == NULL) {
        return NGX_DECLINED;
    }

    ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                  "cache_tag bootstrap zone \"%V\"", &zone->zone_name);

    if (ngx_http_cache_tag_store_begin_batch(store, cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_add_watch_recursive(store, zone,
            &zone->cache->path->name, cycle, NGX_HTTP_CACHE_TAG_INDEX_DIRECT,
            NULL, 0) != NGX_OK) {
        ngx_http_cache_tag_store_rollback_batch(store, cycle->log);
        return NGX_ERROR;
    }

    state.bootstrap_complete = 1;
    state.last_bootstrap_at = ngx_time();
    if (ngx_http_cache_tag_store_set_zone_state(store, &zone->zone_name, &state,
            cycle->log) != NGX_OK) {
        ngx_http_cache_tag_store_rollback_batch(store, cycle->log);
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_commit_batch(store, cycle->log) != NGX_OK) {
        ngx_http_cache_tag_store_rollback_batch(store, cycle->log);
        return NGX_ERROR;
    }

    return NGX_OK;
}

static void
ngx_http_cache_tag_zone_insert(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node,
                               ngx_rbtree_node_t *sentinel) {
    for (;;) {
        if (node->key < temp->key) {
            if (temp->left == sentinel) {
                temp->left = node;
                break;
            }
            temp = temp->left;
            continue;
        }

        if (node->key > temp->key) {
            if (temp->right == sentinel) {
                temp->right = node;
                break;
            }
            temp = temp->right;
            continue;
        }

        if (((ngx_http_cache_tag_zone_index_t *) node)->zone->cache
                < ((ngx_http_cache_tag_zone_index_t *) temp)->zone->cache) {
            if (temp->left == sentinel) {
                temp->left = node;
                break;
            }
            temp = temp->left;
        } else {
            if (temp->right == sentinel) {
                temp->right = node;
                break;
            }
            temp = temp->right;
        }
    }

    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);

}

static void
ngx_http_cache_tag_watch_insert(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node,
                                ngx_rbtree_node_t *sentinel) {
    for (;;) {
        if (node->key < temp->key) {
            if (temp->left == sentinel) {
                temp->left = node;
                break;
            }
            temp = temp->left;
            continue;
        }

        if (node->key > temp->key) {
            if (temp->right == sentinel) {
                temp->right = node;
                break;
            }
            temp = temp->right;
            continue;
        }

        if (((ngx_http_cache_tag_watch_t *) node)->wd
                < ((ngx_http_cache_tag_watch_t *) temp)->wd) {
            if (temp->left == sentinel) {
                temp->left = node;
                break;
            }
            temp = temp->left;
        } else {
            if (temp->right == sentinel) {
                temp->right = node;
                break;
            }
            temp = temp->right;
        }
    }

    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);

}

static ngx_http_cache_tag_zone_index_t *
ngx_http_cache_tag_lookup_zone_index(ngx_http_file_cache_t *cache) {
    ngx_rbtree_node_t              *node, *sentinel;
    ngx_rbtree_key_t                key;
    ngx_http_cache_tag_zone_index_t *index;

    node = ngx_http_cache_tag_watch_runtime.zone_index.root;
    sentinel = ngx_http_cache_tag_watch_runtime.zone_index.sentinel;
    key = (ngx_rbtree_key_t)(uintptr_t) cache;

    while (node != sentinel) {
        if (key < node->key) {
            node = node->left;
            continue;
        }

        if (key > node->key) {
            node = node->right;
            continue;
        }

        index = (ngx_http_cache_tag_zone_index_t *) node;
        if (index->zone->cache == cache) {
            return index;
        }

        node = cache < index->zone->cache ? node->left : node->right;
    }

    return NULL;
}

static ngx_http_cache_tag_watch_t *
ngx_http_cache_tag_find_watch(int wd) {
    ngx_rbtree_node_t           *node, *sentinel;
    ngx_rbtree_key_t             key;
    ngx_http_cache_tag_watch_t  *watch;

    node = ngx_http_cache_tag_watch_runtime.watch_index.root;
    sentinel = ngx_http_cache_tag_watch_runtime.watch_index.sentinel;
    key = (ngx_rbtree_key_t) wd;

    while (node != sentinel) {
        if (key < node->key) {
            node = node->left;
            continue;
        }

        if (key > node->key) {
            node = node->right;
            continue;
        }

        watch = (ngx_http_cache_tag_watch_t *) node;
        if (watch->wd == wd) {
            return watch;
        }

        node = wd < watch->wd ? node->left : node->right;
    }

    return NULL;
}

static ngx_int_t
ngx_http_cache_tag_remove_watch(int wd) {
    ngx_http_cache_tag_watch_t  *watch;

    watch = ngx_http_cache_tag_find_watch(wd);
    if (watch == NULL) {
        return NGX_OK;
    }

    inotify_rm_watch(ngx_http_cache_tag_watch_runtime.inotify_conn->fd, wd);
    ngx_rbtree_delete(&ngx_http_cache_tag_watch_runtime.watch_index,
                      &watch->node);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_add_watch(ngx_http_cache_tag_zone_t *zone, ngx_str_t *path,
                             ngx_cycle_t *cycle) {
    ngx_http_cache_tag_watch_t  *watch;
    int                          wd;

    if (ngx_http_cache_tag_watch_runtime.inotify_conn == NULL) {
        return NGX_DECLINED;
    }

    wd = inotify_add_watch(ngx_http_cache_tag_watch_runtime.inotify_conn->fd,
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

    watch = ngx_pcalloc(cycle->pool, sizeof(ngx_http_cache_tag_watch_t));
    if (watch == NULL) {
        return NGX_ERROR;
    }

    watch->wd = wd;
    watch->node.key = (ngx_rbtree_key_t) wd;
    watch->cache = zone->cache;
    watch->zone_name = zone->zone_name;
    watch->path.len = path->len;
    watch->path.data = ngx_pnalloc(cycle->pool, path->len + 1);
    if (watch->path.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(watch->path.data, path->data, path->len);
    watch->path.data[path->len] = '\0';

    ngx_rbtree_insert(&ngx_http_cache_tag_watch_runtime.watch_index,
                      &watch->node);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_add_watch_recursive(ngx_http_cache_tag_store_t *store,
                                       ngx_http_cache_tag_zone_t *zone,
                                       ngx_str_t *path, ngx_cycle_t *cycle,
                                       ngx_uint_t index_mode,
                                       ngx_array_t *pending_ops,
                                       time_t min_mtime) {
    DIR            *dir;
    struct dirent  *entry;
    ngx_str_t       child;
    ngx_file_info_t fi;

    if (ngx_http_cache_tag_add_watch(zone, path, cycle) == NGX_ERROR) {
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
            if (ngx_http_cache_tag_add_watch_recursive(store, zone, &child, cycle,
                    index_mode, pending_ops, min_mtime) == NGX_ERROR) {
                closedir(dir);
                return NGX_ERROR;
            }
            continue;
        }

        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) {
            continue;
        }

        if (min_mtime > 0) {
            if (ngx_file_info(child.data, &fi) == NGX_FILE_ERROR) {
                continue;
            }

            if (ngx_file_mtime(&fi) <= min_mtime) {
                continue;
            }
        }

        if (index_mode == NGX_HTTP_CACHE_TAG_INDEX_DIRECT) {
            if (ngx_http_cache_tag_store_process_file(store, &zone->zone_name,
                    &child, zone->headers, cycle->log) == NGX_ERROR) {
                closedir(dir);
                return NGX_ERROR;
            }
            continue;
        }

        if (index_mode == NGX_HTTP_CACHE_TAG_INDEX_PENDING
                && ngx_http_cache_tag_pending_op_set(cycle->pool, pending_ops,
                        &zone->zone_name, zone->cache, &child,
                        NGX_HTTP_CACHE_TAG_OP_REPLACE) != NGX_OK) {
            closedir(dir);
            return NGX_ERROR;
        }
    }

    closedir(dir);

    return NGX_OK;
}

/* Read all pending inotify events into pending_ops.  pool is used for path
 * string allocation.  Returns NGX_OK even if no events were available. */
static ngx_int_t
ngx_http_cache_tag_read_inotify(ngx_pool_t *pool, ngx_array_t *pending_ops,
                                ngx_cycle_t *cycle) {
    u_char                      buf[8192];
    ssize_t                     n;
    size_t                      offset;
    struct inotify_event       *event;
    ngx_http_cache_tag_watch_t *watch;
    ngx_http_cache_tag_zone_t   zone;
    ngx_str_t                   path;

    if (ngx_http_cache_tag_watch_runtime.inotify_conn == NULL) {
        return NGX_OK;
    }

    for (;;) {
        n = read(ngx_http_cache_tag_watch_runtime.inotify_conn->fd,
                 buf, sizeof(buf));
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            ngx_log_error(NGX_LOG_ERR, cycle->log, ngx_errno,
                          "cache_tag inotify read failed");
            return NGX_ERROR;
        }

        if (n == 0) {
            break;
        }

        offset = 0;
        while (offset < (size_t) n) {
            event = (struct inotify_event *)(buf + offset);
            watch = ngx_http_cache_tag_find_watch(event->wd);
            if (watch != NULL) {
                if (event->mask & (IN_DELETE_SELF|IN_IGNORED)) {
                    ngx_http_cache_tag_remove_watch(event->wd);
                } else if (event->len > 0
                           && ngx_http_cache_tag_join_path(pool, &watch->path,
                                   event->name, &path) == NGX_OK) {
                    if (event->mask & IN_ISDIR) {
                        if (event->mask & (IN_CREATE|IN_MOVED_TO)) {
                            ngx_http_cache_tag_zone_t *watch_zone;

                            zone.zone_name = watch->zone_name;
                            zone.cache = watch->cache;
                            zone.headers = NULL;
                            watch_zone = ngx_http_cache_tag_lookup_zone(watch->cache);
                            if (watch_zone != NULL) {
                                zone.headers = watch_zone->headers;
                            }

                            if (ngx_http_cache_tag_add_watch_recursive(
                                        ngx_http_cache_tag_store_writer(), &zone,
                                        &path, cycle,
                                        NGX_HTTP_CACHE_TAG_INDEX_PENDING,
                                        pending_ops, 0) == NGX_ERROR) {
                                return NGX_ERROR;
                            }
                        }
                    } else if (event->mask
                               & (IN_CREATE|IN_MOVED_TO|IN_CLOSE_WRITE)) {
                        if (ngx_http_cache_tag_pending_op_set(pool, pending_ops,
                                                              &watch->zone_name, watch->cache, &path,
                                                              NGX_HTTP_CACHE_TAG_OP_REPLACE) != NGX_OK) {
                            return NGX_ERROR;
                        }
                    } else if (event->mask & (IN_DELETE|IN_MOVED_FROM)) {
                        if (ngx_http_cache_tag_pending_op_set(pool, pending_ops,
                                                              &watch->zone_name, watch->cache, &path,
                                                              NGX_HTTP_CACHE_TAG_OP_DELETE) != NGX_OK) {
                            return NGX_ERROR;
                        }
                    }
                }
            }

            offset += sizeof(struct inotify_event) + event->len;
        }
    }

    return NGX_OK;
}

/* Inotify read event handler: fires immediately when the kernel has events.
 * Reads all available events into the persistent runtime pending_ops; does
 * NOT write to the backing store (no blocking I/O in this handler).
 * The timer handler applies pending_ops to the store every 250 ms. */
static void
ngx_http_cache_tag_inotify_handler(ngx_event_t *ev) {
    ngx_cycle_t  *cycle = ngx_http_cache_tag_watch_runtime.cycle;

    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    if (ngx_http_cache_tag_read_inotify(
                ngx_http_cache_tag_watch_runtime.pending_pool,
                ngx_http_cache_tag_watch_runtime.pending_ops,
                cycle) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "cache_tag inotify handler failed");
    }
}

/* Timer handler: drain the inter-worker shm queue, apply all accumulated
 * pending ops to the backing store, then reset the pending_pool so memory
 * from the previous batch is reclaimed. */
static void
ngx_http_cache_tag_timer_handler(ngx_event_t *ev) {
    ngx_cycle_t  *cycle;
    ngx_pool_t   *new_pool;

    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    cycle = ngx_http_cache_tag_watch_runtime.cycle;

    if (ngx_http_cache_tag_queue_drain(
                ngx_http_cache_tag_watch_runtime.pending_pool,
                ngx_http_cache_tag_watch_runtime.pending_ops,
                cycle->log) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "cache_tag queue drain failed");
    }

    if (ngx_http_cache_tag_watch_runtime.pending_ops->nelts > 0
            && ngx_http_cache_tag_apply_pending_ops(
                ngx_http_cache_tag_store_writer(),
                ngx_http_cache_tag_watch_runtime.pending_ops,
                cycle->log) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "cache_tag apply pending ops failed");
    }

    /* Reset the pool so path/zone-name strings from this batch are freed.
     * Create the new pool before destroying the old one so that a single
     * allocation failure does not lose the pending_ops pointer. */
    new_pool = ngx_create_pool(4096, cycle->log);
    if (new_pool != NULL) {
        ngx_destroy_pool(ngx_http_cache_tag_watch_runtime.pending_pool);
        ngx_http_cache_tag_watch_runtime.pending_pool = new_pool;
        ngx_http_cache_tag_watch_runtime.pending_ops =
            ngx_array_create(new_pool, 8,
                             sizeof(ngx_http_cache_tag_pending_op_t));
        if (ngx_http_cache_tag_watch_runtime.pending_ops == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                          "cache_tag failed to recreate pending_ops array");
        }
    }

    ngx_add_timer(ev, 250);
}

/* One-shot flush used by flush_pending() and shutdown: reads any remaining
 * inotify events, drains the shm queue, and applies all ops to the store.
 * Uses the runtime pending_pool/ops so nothing accumulated by the event
 * handler is lost. */
static ngx_int_t
ngx_http_cache_tag_process_events(ngx_cycle_t *cycle) {
    ngx_pool_t  *new_pool;

    if (ngx_http_cache_tag_read_inotify(
                ngx_http_cache_tag_watch_runtime.pending_pool,
                ngx_http_cache_tag_watch_runtime.pending_ops,
                cycle) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_queue_drain(
                ngx_http_cache_tag_watch_runtime.pending_pool,
                ngx_http_cache_tag_watch_runtime.pending_ops,
                cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_watch_runtime.pending_ops->nelts > 0
            && ngx_http_cache_tag_apply_pending_ops(
                ngx_http_cache_tag_store_writer(),
                ngx_http_cache_tag_watch_runtime.pending_ops,
                cycle->log) != NGX_OK) {
        return NGX_ERROR;
    }

    /* Reset for the next batch. */
    new_pool = ngx_create_pool(4096, cycle->log);
    if (new_pool != NULL) {
        ngx_destroy_pool(ngx_http_cache_tag_watch_runtime.pending_pool);
        ngx_http_cache_tag_watch_runtime.pending_pool = new_pool;
        ngx_http_cache_tag_watch_runtime.pending_ops =
            ngx_array_create(new_pool, 8,
                             sizeof(ngx_http_cache_tag_pending_op_t));
    }

    return NGX_OK;
}

/* Arms the inotify epoll event and starts the 250 ms timer.  Called from the
 * synchronous bootstrap path and from the thread-pool completion handler. */
static ngx_int_t
ngx_http_cache_tag_arm_watch(ngx_cycle_t *cycle) {
    ngx_http_cache_tag_watch_runtime.inotify_conn->read->handler =
        ngx_http_cache_tag_inotify_handler;
    ngx_http_cache_tag_watch_runtime.inotify_conn->read->log = cycle->log;
    if (ngx_add_event(ngx_http_cache_tag_watch_runtime.inotify_conn->read,
                      NGX_READ_EVENT, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "cache_tag failed to add inotify read event");
        return NGX_ERROR;
    }

    ngx_http_cache_tag_watch_runtime.timer.handler =
        ngx_http_cache_tag_timer_handler;
    ngx_http_cache_tag_watch_runtime.timer.log = cycle->log;
    ngx_http_cache_tag_watch_runtime.timer.data = NULL;
    ngx_http_cache_tag_watch_runtime.timer.cancelable = 1;
    ngx_add_timer(&ngx_http_cache_tag_watch_runtime.timer, 250);

    return NGX_OK;
}

#if (NGX_CACHE_PURGE_THREADS)

typedef struct {
    ngx_cycle_t                       *cycle;
    ngx_http_cache_purge_main_conf_t  *pmcf;
    ngx_int_t                          rc;
} ngx_http_cache_tag_bootstrap_ctx_t;

/* Thread function: runs the full per-zone bootstrap (dir walk + tag writes)
 * using a dedicated store connection.  The event loop's writer stays idle
 * until the completion handler starts the timer — no locking needed. */
static void
ngx_http_cache_tag_bootstrap_thread(void *data, ngx_log_t *log) {
    ngx_http_cache_tag_bootstrap_ctx_t  *ctx;
    ngx_http_cache_tag_store_t          *writer;
    ngx_http_cache_tag_zone_t           *zone;
    ngx_http_cache_tag_zone_state_t      state;
    ngx_uint_t                           i;

    ctx = data;

    writer = ngx_http_cache_tag_store_open_writer(ctx->pmcf, log);
    if (writer == NULL) {
        ctx->rc = NGX_ERROR;
        return;
    }

    zone = ctx->pmcf->zones->elts;
    for (i = 0; i < ctx->pmcf->zones->nelts; i++) {
        if (zone[i].cache == NULL || zone[i].cache->path == NULL) {
            continue;
        }

        if (ngx_http_cache_tag_store_get_zone_state(writer, &zone[i].zone_name,
                &state, log) != NGX_OK) {
            ctx->rc = NGX_ERROR;
            ngx_http_cache_tag_store_close(writer);
            return;
        }

        if (state.bootstrap_complete) {
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag reusing persisted index for zone \"%V\"",
                          &zone[i].zone_name);
            if (ngx_http_cache_tag_add_watch_recursive(writer, &zone[i],
                    &zone[i].cache->path->name, ctx->cycle,
                    NGX_HTTP_CACHE_TAG_INDEX_DIRECT, NULL,
                    state.last_bootstrap_at) == NGX_ERROR) {
                ctx->rc = NGX_ERROR;
                ngx_http_cache_tag_store_close(writer);
                return;
            }

            state.last_bootstrap_at = ngx_time();
            if (ngx_http_cache_tag_store_set_zone_state(writer,
                    &zone[i].zone_name, &state, log) != NGX_OK) {
                ctx->rc = NGX_ERROR;
                ngx_http_cache_tag_store_close(writer);
                return;
            }
            continue;
        }

        if (ngx_http_cache_tag_bootstrap_zone(writer, &zone[i],
                                              ctx->cycle) != NGX_OK) {
            ctx->rc = NGX_ERROR;
            ngx_http_cache_tag_store_close(writer);
            return;
        }
    }

    ngx_http_cache_tag_store_close(writer);
    ctx->rc = NGX_OK;
}

/* Completion handler: called in the event loop after the bootstrap thread
 * finishes.  Arms inotify and starts the timer. */
static void
ngx_http_cache_tag_bootstrap_complete(ngx_event_t *ev) {
    ngx_http_cache_tag_bootstrap_ctx_t  *ctx;
    ngx_cycle_t                         *cycle;

    ctx = ev->data;
    cycle = ctx->cycle;

    if (ctx->rc != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "cache_tag: background bootstrap failed, "
                      "continuing with partial index");
        /* Non-fatal: inotify will track changes going forward. */
    }

    if (ngx_http_cache_tag_arm_watch(cycle) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "cache_tag: failed to arm inotify after bootstrap");
        return;
    }

    ngx_http_cache_tag_watch_runtime.active = 1;
}

#endif /* NGX_CACHE_PURGE_THREADS */

ngx_int_t
ngx_http_cache_tag_init_runtime(ngx_cycle_t *cycle,
                                ngx_http_cache_purge_main_conf_t *pmcf) {
    ngx_http_cache_tag_zone_t        *zone;
    ngx_http_cache_tag_store_t       *writer;
    ngx_http_cache_tag_zone_state_t   state;
    ngx_uint_t                        i;
#if (NGX_CACHE_PURGE_THREADS)
    ngx_thread_pool_t                *tp;
    ngx_thread_task_t                *task;
    ngx_http_cache_tag_bootstrap_ctx_t *bctx;
    static ngx_str_t                  default_pool_name = ngx_string("default");
#endif

    ngx_memzero(&ngx_http_cache_tag_watch_runtime,
                sizeof(ngx_http_cache_tag_watch_runtime));

    ngx_rbtree_init(&ngx_http_cache_tag_watch_runtime.zone_index,
                    &ngx_http_cache_tag_watch_runtime.zone_sentinel,
                    (ngx_rbtree_insert_pt) ngx_http_cache_tag_zone_insert);
    ngx_rbtree_init(&ngx_http_cache_tag_watch_runtime.watch_index,
                    &ngx_http_cache_tag_watch_runtime.watch_sentinel,
                    (ngx_rbtree_insert_pt) ngx_http_cache_tag_watch_insert);

    ngx_http_cache_tag_watch_runtime.owner = (
                ngx_process == NGX_PROCESS_WORKER && ngx_worker == 0);
    ngx_http_cache_tag_watch_runtime.cycle = cycle;
    ngx_http_cache_tag_queue_ctx = pmcf->queue_zone != NULL
                                   ? pmcf->queue_zone->data : NULL;

    if (ngx_http_cache_tag_runtime_init_zones(cycle, pmcf) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_runtime_init(cycle, pmcf,
            ngx_http_cache_tag_watch_runtime.owner) != NGX_OK) {
        return NGX_ERROR;
    }

    if (!ngx_http_cache_tag_watch_runtime.owner || pmcf->zones->nelts == 0) {
        ngx_http_cache_tag_watch_runtime.initialized = 1;
        return NGX_OK;
    }

    {
        int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd == -1) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, ngx_errno,
                          "inotify_init1 failed");
            return NGX_ERROR;
        }

        ngx_http_cache_tag_watch_runtime.inotify_conn =
            ngx_get_connection(inotify_fd, cycle->log);
        if (ngx_http_cache_tag_watch_runtime.inotify_conn == NULL) {
            close(inotify_fd);
            return NGX_ERROR;
        }
        ngx_http_cache_tag_watch_runtime.inotify_conn->log = cycle->log;
    }

    /* Persistent pool and op-list for the event-driven inotify read path.
     * The inotify_handler appends to these; the timer handler applies and
     * resets them. */
    ngx_http_cache_tag_watch_runtime.pending_pool =
        ngx_create_pool(4096, cycle->log);
    if (ngx_http_cache_tag_watch_runtime.pending_pool == NULL) {
        return NGX_ERROR;
    }

    ngx_http_cache_tag_watch_runtime.pending_ops = ngx_array_create(
                ngx_http_cache_tag_watch_runtime.pending_pool, 8,
                sizeof(ngx_http_cache_tag_pending_op_t));
    if (ngx_http_cache_tag_watch_runtime.pending_ops == NULL) {
        return NGX_ERROR;
    }

#if (NGX_CACHE_PURGE_THREADS)
    /* Try to offload the blocking bootstrap walk to the default thread pool.
     * If the thread pool is unavailable (nginx not built with --with-threads,
     * or no "thread_pool default" directive), fall through to the sync path. */
    tp = ngx_thread_pool_get(cycle, &default_pool_name);
    if (tp != NULL) {
        task = ngx_thread_task_alloc(cycle->pool,
                                     sizeof(ngx_http_cache_tag_bootstrap_ctx_t));
        if (task != NULL) {
            bctx = task->ctx;
            bctx->cycle = cycle;
            bctx->pmcf  = pmcf;
            bctx->rc    = NGX_OK;
            task->handler       = ngx_http_cache_tag_bootstrap_thread;
            task->event.handler = ngx_http_cache_tag_bootstrap_complete;
            task->event.data    = bctx;
            if (ngx_thread_task_post(tp, task) == NGX_OK) {
                /* Async path: arm_watch and active=1 happen in completion
                 * handler once the thread finishes. */
                ngx_http_cache_tag_watch_runtime.initialized = 1;
                return NGX_OK;
            }
        }
    }
    /* Thread pool not available or post failed — fall through to sync path. */
#endif

    writer = ngx_http_cache_tag_store_writer();
    zone = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        if (zone[i].cache == NULL || zone[i].cache->path == NULL) {
            continue;
        }

        if (ngx_http_cache_tag_store_get_zone_state(writer, &zone[i].zone_name,
                &state, cycle->log) != NGX_OK) {
            return NGX_ERROR;
        }

        if (state.bootstrap_complete) {
            ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0,
                          "cache_tag reusing persisted index for zone \"%V\"",
                          &zone[i].zone_name);
            if (ngx_http_cache_tag_add_watch_recursive(writer, &zone[i],
                    &zone[i].cache->path->name, cycle,
                    NGX_HTTP_CACHE_TAG_INDEX_DIRECT, NULL,
                    state.last_bootstrap_at) == NGX_ERROR) {
                return NGX_ERROR;
            }

            state.last_bootstrap_at = ngx_time();
            if (ngx_http_cache_tag_store_set_zone_state(writer, &zone[i].zone_name,
                    &state, cycle->log) != NGX_OK) {
                return NGX_ERROR;
            }
            continue;
        }

        if (ngx_http_cache_tag_bootstrap_zone(writer, &zone[i], cycle)
                != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (ngx_http_cache_tag_arm_watch(cycle) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_http_cache_tag_watch_runtime.initialized = 1;
    ngx_http_cache_tag_watch_runtime.active = 1;

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_flush_pending(ngx_cycle_t *cycle) {
    if (!ngx_http_cache_tag_watch_runtime.owner
            || !ngx_http_cache_tag_watch_runtime.active) {
        return NGX_OK;
    }

    return ngx_http_cache_tag_process_events(cycle);
}

void
ngx_http_cache_tag_shutdown_runtime(void) {
    if (ngx_http_cache_tag_watch_runtime.owner
            && ngx_http_cache_tag_watch_runtime.active) {
        ngx_http_cache_tag_process_events(ngx_http_cache_tag_watch_runtime.cycle);
    }

    if (ngx_http_cache_tag_watch_runtime.timer.timer_set) {
        ngx_del_timer(&ngx_http_cache_tag_watch_runtime.timer);
    }

    if (ngx_http_cache_tag_watch_runtime.inotify_conn != NULL) {
        ngx_del_event(ngx_http_cache_tag_watch_runtime.inotify_conn->read,
                      NGX_READ_EVENT, 0);
        ngx_close_connection(ngx_http_cache_tag_watch_runtime.inotify_conn);
        ngx_http_cache_tag_watch_runtime.inotify_conn = NULL;
    }

    if (ngx_http_cache_tag_watch_runtime.pending_pool != NULL) {
        ngx_destroy_pool(ngx_http_cache_tag_watch_runtime.pending_pool);
        ngx_http_cache_tag_watch_runtime.pending_pool = NULL;
        ngx_http_cache_tag_watch_runtime.pending_ops = NULL;
    }

    ngx_http_cache_tag_store_runtime_shutdown();

    if (ngx_http_cache_tag_queue_ctx != NULL) {
        ngx_http_cache_tag_queue_log_stats(ngx_http_cache_tag_watch_runtime.cycle->log,
                                           NGX_LOG_NOTICE,
                                           "cache_tag queue shutdown summary");
    }

    ngx_http_cache_tag_queue_ctx = NULL;

    ngx_memzero(&ngx_http_cache_tag_watch_runtime,
                sizeof(ngx_http_cache_tag_watch_runtime));
}

static void
ngx_http_cache_tag_queue_log_stats(ngx_log_t *log, ngx_uint_t level,
                                   const char *message) {
    ngx_uint_t                        dropped;
    ngx_http_cache_tag_queue_ctx_t  *ctx;

    ctx = ngx_http_cache_tag_queue_ctx;
    dropped = 0;

    if (ctx != NULL && ctx->sh != NULL) {
        dropped = ctx->sh->dropped;
    }

    ngx_log_error(level, log, 0,
                  "%s (accepted=%ui rejected=%ui drained=%ui dropped=%ui)",
                  message,
                  ctx != NULL ? ctx->stats.accepted : 0,
                  ctx != NULL ? ctx->stats.rejected : 0,
                  ctx != NULL ? ctx->stats.drained : 0,
                  dropped);
}

static ngx_int_t
ngx_http_cache_tag_queue_init_zone(ngx_shm_zone_t *shm_zone, void *data) {
    ngx_http_cache_tag_queue_ctx_t    *octx = data;
    ngx_http_cache_tag_queue_ctx_t    *ctx;
    ngx_http_cache_tag_queue_shctx_t  *sh;
    size_t                             size;

    ctx = shm_zone->data;

    if (octx != NULL) {
        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;
        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;
        return NGX_OK;
    }

    size = sizeof(ngx_http_cache_tag_queue_shctx_t)
           + (NGX_HTTP_CACHE_TAG_QUEUE_CAPACITY - 1)
           * sizeof(ngx_http_cache_tag_queue_entry_t);

    sh = ngx_slab_calloc(ctx->shpool, size);
    if (sh == NULL) {
        return NGX_ERROR;
    }

    sh->capacity = NGX_HTTP_CACHE_TAG_QUEUE_CAPACITY;
    ctx->shpool->data = sh;
    ctx->sh = sh;

    return NGX_OK;
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

static ngx_int_t
ngx_http_cache_tag_runtime_init_zones(ngx_cycle_t *cycle,
                                      ngx_http_cache_purge_main_conf_t *pmcf) {
    ngx_http_cache_tag_zone_t        *zone;
    ngx_http_cache_tag_zone_index_t  *index;
    ngx_uint_t                        i;

    zone = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        index = ngx_pcalloc(cycle->pool, sizeof(ngx_http_cache_tag_zone_index_t));
        if (index == NULL) {
            return NGX_ERROR;
        }

        index->zone = &zone[i];
        index->node.key = (ngx_rbtree_key_t)(uintptr_t) zone[i].cache;
        ngx_rbtree_insert(&ngx_http_cache_tag_watch_runtime.zone_index,
                          &index->node);
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_pending_op_set(ngx_pool_t *pool, ngx_array_t *pending_ops,
                                  ngx_str_t *zone_name,
                                  ngx_http_file_cache_t *cache,
                                  ngx_str_t *path, ngx_uint_t operation) {
    ngx_http_cache_tag_pending_op_t  *op;
    ngx_uint_t                        i;

    op = pending_ops->elts;
    for (i = 0; i < pending_ops->nelts; i++) {
        if (op[i].zone_name.len == zone_name->len
                && ngx_strncmp(op[i].zone_name.data, zone_name->data,
                               zone_name->len) == 0
                && op[i].path.len == path->len
                && ngx_strncmp(op[i].path.data, path->data, path->len) == 0) {
            op[i].operation = operation;
            if (cache != NULL) {
                op[i].cache = cache;
            }
            return NGX_OK;
        }
    }

    op = ngx_array_push(pending_ops);
    if (op == NULL) {
        return NGX_ERROR;
    }

    op->operation = operation;
    op->cache = cache;
    op->zone_name.len = zone_name->len;
    op->zone_name.data = ngx_pnalloc(pool, zone_name->len + 1);
    if (op->zone_name.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(op->zone_name.data, zone_name->data, zone_name->len);
    op->zone_name.data[zone_name->len] = '\0';

    op->path.len = path->len;
    op->path.data = ngx_pnalloc(pool, path->len + 1);
    if (op->path.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(op->path.data, path->data, path->len);
    op->path.data[path->len] = '\0';

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_queue_drain(ngx_pool_t *pool, ngx_array_t *pending_ops,
                               ngx_log_t *log) {
    ngx_http_cache_tag_queue_shctx_t  *sh;
    ngx_http_cache_tag_queue_entry_t  *entry, *copy;
    ngx_array_t                       *entries;
    ngx_str_t                          zone_name, path;
    ngx_uint_t                         count, i;

    if (ngx_http_cache_tag_queue_ctx == NULL
            || ngx_http_cache_tag_queue_ctx->shpool == NULL
            || ngx_http_cache_tag_queue_ctx->sh == NULL) {
        return NGX_OK;
    }

    sh = ngx_http_cache_tag_queue_ctx->sh;

    ngx_shmtx_lock(&ngx_http_cache_tag_queue_ctx->shpool->mutex);
    count = sh->count;
    entries = ngx_array_create(pool, count > 0 ? count : 1,
                               sizeof(ngx_http_cache_tag_queue_entry_t));
    if (entries == NULL) {
        ngx_shmtx_unlock(&ngx_http_cache_tag_queue_ctx->shpool->mutex);
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "cache_tag queue drain failed while allocating entry copy buffer");
        ngx_http_cache_tag_queue_log_stats(log, NGX_LOG_ERR,
                                           "cache_tag queue drain failed");
        return NGX_ERROR;
    }

    for (i = 0; i < count; i++) {
        entry = &sh->entries[sh->head];
        copy = ngx_array_push(entries);
        if (copy == NULL) {
            ngx_shmtx_unlock(&ngx_http_cache_tag_queue_ctx->shpool->mutex);
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "cache_tag queue drain failed while buffering pending operations");
            ngx_http_cache_tag_queue_log_stats(log, NGX_LOG_ERR,
                                               "cache_tag queue drain failed");
            return NGX_ERROR;
        }

        *copy = *entry;

        sh->head = (sh->head + 1) % sh->capacity;
        sh->count--;
    }

    ngx_shmtx_unlock(&ngx_http_cache_tag_queue_ctx->shpool->mutex);

    entry = entries->elts;
    for (i = 0; i < entries->nelts; i++) {
        zone_name.data = entry[i].zone_name;
        zone_name.len = entry[i].zone_name_len;
        path.data = entry[i].path;
        path.len = entry[i].path_len;

        if (ngx_http_cache_tag_pending_op_set(pool, pending_ops, &zone_name,
                                              NULL, &path, entry[i].operation) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "cache_tag queue drain failed while copying pending operations");
            ngx_http_cache_tag_queue_log_stats(log, NGX_LOG_ERR,
                                               "cache_tag queue drain failed");
            return NGX_ERROR;
        }
    }

    ngx_http_cache_tag_queue_ctx->stats.drained += entries->nelts;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_apply_pending_ops(ngx_http_cache_tag_store_t *store,
                                     ngx_array_t *pending_ops, ngx_log_t *log) {
    ngx_http_cache_tag_pending_op_t  *op;
    ngx_http_cache_tag_zone_t        *zone;
    ngx_uint_t                        i;

    if (pending_ops->nelts == 0) {
        return NGX_OK;
    }

    if (ngx_http_cache_tag_store_begin_batch(store, log) != NGX_OK) {
        return NGX_ERROR;
    }

    op = pending_ops->elts;
    for (i = 0; i < pending_ops->nelts; i++) {
        if (op[i].operation == NGX_HTTP_CACHE_TAG_OP_DELETE) {
            if (ngx_http_cache_tag_store_delete_file(store, &op[i].zone_name,
                    &op[i].path, log) != NGX_OK) {
                ngx_http_cache_tag_store_rollback_batch(store, log);
                return NGX_ERROR;
            }
            continue;
        }

        zone = ngx_http_cache_tag_lookup_zone(op[i].cache);
        if (zone == NULL || zone->headers == NULL) {
            ngx_http_cache_tag_store_rollback_batch(store, log);
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "cache_tag missing header configuration for zone \"%V\"",
                          &op[i].zone_name);
            return NGX_ERROR;
        }

        if (ngx_http_cache_tag_store_process_file(store, &op[i].zone_name,
                &op[i].path, zone->headers, log) != NGX_OK) {
            ngx_http_cache_tag_store_rollback_batch(store, log);
            return NGX_ERROR;
        }
    }

    if (ngx_http_cache_tag_store_commit_batch(store, log) != NGX_OK) {
        ngx_http_cache_tag_store_rollback_batch(store, log);
        return NGX_ERROR;
    }

    return NGX_OK;
}

#endif
