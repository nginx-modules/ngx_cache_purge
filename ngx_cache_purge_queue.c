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

#if (NGX_HTTP_CACHE)

static ngx_http_cache_purge_stats_t *
ngx_http_cache_purge_stats(ngx_http_cache_purge_path_t *path);
static void
ngx_http_cache_purge_background_handler(ngx_event_t *ev);
#if (NGX_THREADS && !NGX_WIN32)
static void
ngx_http_cache_purge_thread(void *data, ngx_log_t *log);
#endif
#if (NGX_THREADS && !NGX_WIN32)
static void
ngx_http_cache_purge_thread_done(ngx_event_t *ev);
#endif
static ngx_msec_t
ngx_http_cache_purge_age(ngx_msec_t since);
static void
ngx_http_cache_purge_recover(ngx_http_cache_purge_queue_t *queue);
static ngx_int_t
ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle, ngx_uint_t final);
static ngx_http_cache_purge_path_t *
ngx_http_cache_purge_path_get(ngx_http_cache_purge_queue_t *queue,
    ngx_str_t *name);
static ngx_int_t
ngx_http_cache_purge_bind_queue_status(ngx_http_cache_purge_main_conf_t *cmcf);
static ngx_http_cache_purge_protection_t *
ngx_http_cache_purge_protection_get(ngx_http_cache_purge_queue_t *queue,
    ngx_array_t *paths);
static void
ngx_http_cache_purge_queue_status_snapshot(ngx_http_cache_purge_queue_t *queue,
    ngx_http_cache_purge_path_t *path, ngx_http_cache_purge_queue_stat_t *snap);

static ngx_http_cache_purge_work_t  ngx_cache_purge_work;
static ngx_uint_t  ngx_cache_purge_slot = NGX_CONF_UNSET_UINT;


static ngx_http_cache_purge_stats_t *
ngx_http_cache_purge_stats(ngx_http_cache_purge_path_t *path)
{
    if (path == NULL || ngx_cache_purge_slot == NGX_CONF_UNSET_UINT) {
        return NULL;
    }
    return (ngx_http_cache_purge_stats_t *)
           (path->stats + ngx_cache_purge_slot * path->stats_stride);
}


void
ngx_http_cache_purge_stat_add(ngx_http_cache_purge_path_t *path,
    ngx_uint_t field, uint64_t value)
{
    ngx_http_cache_purge_stats_t  *stats;
    uint64_t                     old;

    stats = ngx_http_cache_purge_stats(path);
    if (stats == NULL) {
        return;
    }
#if (NGX_PTR_SIZE == 4)
    stats->sequence++;
    ngx_memory_barrier();
#endif
    old = stats->value[field];
    stats->value[field] = old + ngx_min(value, (uint64_t) -1 - old);
#if (NGX_PTR_SIZE == 4)
    ngx_memory_barrier();
    stats->sequence++;
#endif
}


# if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
void
ngx_http_cache_purge_stat_wait(ngx_http_cache_purge_path_t *path, ngx_int_t delta)
{
    ngx_http_cache_purge_stats_t  *stats;

    stats = ngx_http_cache_purge_stats(path);
    if (stats == NULL) {
        return;
    }
#if (NGX_PTR_SIZE == 4)
    stats->sequence++;
    ngx_memory_barrier();
#endif
    if (delta > 0) {
        stats->value[NGX_PURGE_WAITING]++;
    } else if (stats->value[NGX_PURGE_WAITING] != 0) {
        stats->value[NGX_PURGE_WAITING]--;
    }
#if (NGX_PTR_SIZE == 4)
    ngx_memory_barrier();
    stats->sequence++;
#endif
}


# endif


ngx_int_t
ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_cache_purge_main_conf_t  *cmcf = shm_zone->data;
    ngx_http_cache_purge_main_conf_t  *old = data;
    ngx_http_cache_purge_queue_t      *queue;
    ngx_slab_pool_t                   *shpool;
    ngx_int_t                          rc;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (old != NULL) {
        if (cmcf->queue_size != old->queue_size) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "ngx_cache_purge: cache_purge_queue_size changes "
                          "from %ui to %ui; "
                          "restart nginx to change capacity",
                          old->queue_size, cmcf->queue_size);
            return NGX_ERROR;
        }

        queue = old->queue;

    } else if (shm_zone->shm.exists) {
        /* Windows workers attach to the zone initialized by the master. */
        queue = shpool->data;

    } else {
        queue = ngx_slab_calloc(shpool, sizeof(ngx_http_cache_purge_queue_t));
        if (queue == NULL) {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "ngx_cache_purge: could not allocate queue "
                          "in shared memory zone \"%V\"", &shm_zone->shm.name);
            return NGX_ERROR;
        }

        queue->version = NGX_CACHE_PURGE_SHM_VERSION;
        ngx_queue_init(&queue->pending);
        ngx_queue_init(&queue->active);
        queue->shpool = shpool;
        shpool->data = queue;
    }

    if (queue == NULL || queue->version != NGX_CACHE_PURGE_SHM_VERSION) {
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                      "ngx_cache_purge: incompatible shared zone; restart "
                      "nginx");
        return NGX_ERROR;
    }

    cmcf->queue = queue;

    ngx_shmtx_lock(&queue->shpool->mutex);

    cmcf->protection = ngx_http_cache_purge_protection_get(queue,
                                                        cmcf->paths);
    rc = NGX_ERROR;
    if (cmcf->protection != NULL) {
        rc = ngx_http_cache_purge_bind_queue_status(cmcf);
    }

    if (rc == NGX_OK) {
        queue->max_size = cmcf->queue_size;
    }

    ngx_shmtx_unlock(&queue->shpool->mutex);

    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                      "ngx_cache_purge: could not allocate path metadata in "
                      "zone \"%V\" of %uz bytes; increase "
                      "cache_purge_queue_size and restart nginx",
                      &shm_zone->shm.name,
                      shm_zone->shm.size);
    }

    return rc;
}


/* -- worker lifecycle --------------------------------------------------- */


ngx_int_t
ngx_http_cache_purge_init_worker(ngx_cycle_t *cycle)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_work_t       *work;
    ngx_http_cache_purge_path_t       *path;
    ngx_http_cache_purge_stats_t      *stats;
    ngx_uint_t                        slot;
#if (NGX_THREADS && !NGX_WIN32)
    ngx_core_conf_t                  *ccf;
    ngx_flag_t                       use_threads;
#endif

    if (ngx_process != NGX_PROCESS_WORKER
        && ngx_process != NGX_PROCESS_SINGLE)
    {
        return NGX_OK;
    }

    if (ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module)
        == NULL)
    {
        return NGX_OK;
    }

    cmcf = ngx_http_cycle_get_module_main_conf(cycle,
                                              ngx_http_cache_purge_module);
    if (cmcf == NULL || cmcf->queue == NULL) {
        return NGX_OK;
    }

    ngx_shmtx_lock(&cmcf->queue->shpool->mutex);
#if (NGX_WIN32)
    for (slot = 0; slot < NGX_MAX_PROCESSES; slot++) {
        if (cmcf->queue->owners[slot] == 0
            || cmcf->queue->owners[slot] == ngx_pid
            || !ngx_http_cache_purge_process_alive(cmcf->queue->owners[slot]))
        {
            break;
        }
    }
#else
    slot = (ngx_process == NGX_PROCESS_SINGLE) ? 0 : (ngx_uint_t) ngx_process_slot;
#endif
    if (slot >= NGX_MAX_PROCESSES) {
        ngx_shmtx_unlock(&cmcf->queue->shpool->mutex);
        return NGX_ERROR;
    }
    if (ngx_cache_purge_slot != slot || cmcf->queue->owners[slot] != ngx_pid) {
        if (cmcf->queue->epochs[slot] == (uint64_t) -1) {
            ngx_shmtx_unlock(&cmcf->queue->shpool->mutex);
            return NGX_ERROR;
        }
        cmcf->queue->epochs[slot]++;
    }
    ngx_cache_purge_slot = slot;
    cmcf->queue->owners[slot] = ngx_pid;
    cmcf->queue->probes[slot] = 0;
    for (path = cmcf->queue->paths; path != NULL; path = path->next) {
        stats = ngx_http_cache_purge_stats(path);
        stats->sequence |= 1;
        ngx_memory_barrier();
        stats->value[NGX_PURGE_WAITING] = 0;
        ngx_memory_barrier();
        stats->sequence++;
    }
    ngx_shmtx_unlock(&cmcf->queue->shpool->mutex);

    work = &ngx_cache_purge_work;
    work->conf = cmcf;
    if (work->event.timer_set) {
        ngx_del_timer(&work->event);
    }

    work->event.handler = ngx_http_cache_purge_background_handler;
    work->event.log = cycle->log;
    work->event.data = cycle;
    work->event.cancelable = (work->item == NULL);
    work->ctx.log = cycle->log;

#if (NGX_THREADS && !NGX_WIN32)
    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    use_threads = ccf->master;
#if (nginx_version < 1007012 && NGX_HAVE_EPOLL)
    /* 1.7.11 publishes the epoll callback after waking the event loop.
     * Use the event-driven walker until core's 1.7.12 ordering fix. */
    if (use_threads && (ngx_event_flags & NGX_USE_EPOLL_EVENT)) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "ngx_cache_purge: nginx before 1.7.12 has an epoll "
                      "thread notification race; using incremental execution");
        use_threads = 0;
    }
#endif
    if (use_threads && work->task == NULL) {
        work->task = ngx_thread_task_alloc(cycle->pool, 0);
        if (work->task == NULL) {
            return NGX_ERROR;
        }
        work->task->ctx = work;
        work->task->handler = ngx_http_cache_purge_thread;
        work->task->event.handler = ngx_http_cache_purge_thread_done;
        work->task->event.data = cycle;
        work->task->event.log = cycle->log;
    }
#endif

    ngx_add_timer(&work->event, cmcf->throttle_ms);

    return NGX_OK;
}


void
ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle)
{
    ngx_http_cache_purge_work_t  *work;

    work = &ngx_cache_purge_work;
    if (work->event.timer_set) {
        ngx_del_timer(&work->event);
    }

#if (NGX_THREADS && !NGX_WIN32)
    /* A failed core thread exit may leave file I/O in flight. */
    if (work->task != NULL && work->task->event.active && !work->finished) {
        return;
    }
    ngx_memory_barrier();
#endif

    if (work->item != NULL) {
        (void) ngx_http_cache_purge_process_queue(cycle, 1);
    }

    if (work->conf != NULL && ngx_cache_purge_slot != NGX_CONF_UNSET_UINT) {
        ngx_shmtx_lock(&work->conf->queue->shpool->mutex);
        work->conf->queue->owners[ngx_cache_purge_slot] = 0;
        work->conf->queue->probes[ngx_cache_purge_slot] = 0;
        ngx_shmtx_unlock(&work->conf->queue->shpool->mutex);
    }
    if (work->cursor.path.data != NULL) {
        ngx_free(work->cursor.path.data);
        ngx_str_null(&work->cursor.path);
        work->cursor.allocated = 0;
    }
}


static void
ngx_http_cache_purge_background_handler(ngx_event_t *ev)
{
    ngx_cycle_t                  *cycle = ev->data;
    ngx_http_cache_purge_work_t   *work = &ngx_cache_purge_work;
    ngx_int_t                     rc;
    ngx_msec_t                    start, delay;

    start = ngx_current_msec;
    delay = 1;

    if (ngx_http_cache_purge_age(work->recover_at) >= 1000) {
        work->recover_at = ngx_current_msec;
        ngx_http_cache_purge_recover(work->conf->queue);
    }

    /* Keep batch progress across timer slices and thread completions. */
    for ( ;; ) {
        rc = ngx_http_cache_purge_process_queue(cycle, 0);

        if (rc == NGX_DONE) {
            return;
        }

        if (rc == NGX_OK) {
            work->batch = 0;
            delay = work->conf->throttle_ms * 10;
            break;
        }

        if (rc == NGX_BUSY) {
            if (work->item == NULL) {
                delay = 10;
            }
#if (NGX_THREADS && !NGX_WIN32)
            if (work->pool != NULL && !work->cursor.started) {
                delay = 10;
            }
#endif
            break;
        }

        if (++work->batch >= work->conf->batch_size) {
            work->batch = 0;
            delay = work->conf->throttle_ms;
            break;
        }

        ngx_time_update();
        if ((ngx_msec_int_t) (ngx_current_msec - start)
            >= NGX_CACHE_PURGE_WALK_MSEC)
        {
            break;
        }
    }

    if (work->item == NULL && (ngx_exiting || ngx_terminate || ngx_quit)) {
        return;
    }

    ngx_add_timer(ev, delay);
}


#if (NGX_THREADS && !NGX_WIN32)

static void
ngx_http_cache_purge_thread(void *data, ngx_log_t *log)
{
    ngx_http_cache_purge_work_t  *work = data;

    (void) log;
    (void) ngx_http_cache_purge_walk_step(&work->cursor, 0);
    work->cursor.complete = 1;
    ngx_memory_barrier();
    work->finished = 1;
}


static void
ngx_http_cache_purge_thread_done(ngx_event_t *ev)
{
    ngx_http_cache_purge_work_t  *work;

    work = &ngx_cache_purge_work;
    if (ev->timedout) {
        ev->timedout = 0;
        ngx_add_timer(ev, NGX_CACHE_PURGE_QUEUE_TIMEOUT);
        return;
    }

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    ngx_http_cache_purge_background_handler(&work->event);
}

#endif


/* -- queue operations --------------------------------------------------- */

# if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)


ngx_int_t
ngx_http_cache_purge_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all, ngx_str_t *expected)
{
    ngx_http_cache_purge_main_conf_t   *cmcf;
    ngx_http_cache_purge_loc_conf_t    *cplcf;
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *item;
    ngx_http_cache_purge_path_t        *path;
    ngx_str_node_t                    *node;
    uint32_t                           hash;
    size_t                             key_len;
    ngx_str_t                          lookup_key;
    u_char                            *p;
    u_char                             index[NGX_CACHE_PURGE_KEY_MAX_LEN + 1];
    ngx_int_t                          rc;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (cmcf == NULL || cmcf->queue == NULL) {
        return NGX_ERROR;
    }
    if (ngx_exiting || ngx_terminate || ngx_quit) {
        return NGX_ABORT;
    }

    key_len = purge_all ? 0 : key->len;
    if ((cplcf->namespace == NULL && key_len > NGX_CACHE_PURGE_KEY_MAX_LEN)
        || (cplcf->namespace != NULL
            && key->len > NGX_CACHE_PURGE_LOGICAL_KEY_MAX))
    {
        return NGX_DECLINED;
    }
    queue = cmcf->queue;
    lookup_key.data = index;
    if (cplcf->namespace != NULL) {
        index[0] = 'G';
        key_len = cplcf->namespace->name.len;
        ngx_memcpy(index + 1, cplcf->namespace->name.data, key_len);
    } else {
        index[0] = 'L';
        ngx_memcpy(index + 1, key->data, key_len);
    }
    lookup_key.len = ++key_len;
    hash = ngx_crc32_short(lookup_key.data, lookup_key.len);

    if (!ngx_shmtx_trylock(&queue->shpool->mutex)) {
        return NGX_AGAIN;
    }
    for (path = queue->paths; path != NULL; path = path->next) {
        if (path->name.len == ngx_http_cache_purge_path_length(&cache->path->name)
            && ngx_http_cache_purge_path_equal(path->name.data,
                                    cache->path->name.data, path->name.len))
        {
            break;
        }
    }
    if (path == NULL) {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_AGAIN;
    }
    node = ngx_str_rbtree_lookup(&path->rbtree, &lookup_key, hash);
    if (node != NULL) {
        if (cplcf->namespace != NULL) {
            rc = ngx_http_cache_purge_generation_invalidate(cplcf->namespace,
                                                           key, purge_all, expected);
            if (rc != NGX_OK) {
                ngx_shmtx_unlock(&queue->shpool->mutex);
                return rc;
            }
        }
        item = (ngx_http_cache_purge_queue_item_t *) node;
        if (item->running) {
            /* One replay covers all invalidations received during this pass. */
            item->rerun = 1;
            item->next_protection = cmcf->protection;
        } else {
            item->protection = cmcf->protection;
        }
        /* A fresh request starts a fresh bounded retry window. */
        item->retries = 0;
        item->retry_at = 0;
        ngx_shmtx_unlock(&queue->shpool->mutex);
        ngx_http_cache_purge_stat_add(path, NGX_PURGE_DEDUPLICATED, 1);
        if (cplcf->namespace != NULL) {
            ngx_http_cache_purge_stat_add(path, NGX_PURGE_INVALIDATED, 1);
        }
        return NGX_OK;
    }

    /* Active work retains its admission credit until final completion. */
    if (queue->size + queue->running >= queue->max_size) {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_AGAIN;
    }

    item = ngx_slab_calloc_locked(queue->shpool, sizeof(*item) + key_len + 1
                                              + cplcf->thread_pool_name.len);
    if (item == NULL) {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_AGAIN;
    }
    if (cplcf->namespace != NULL) {
        rc = ngx_http_cache_purge_generation_invalidate(cplcf->namespace,
                                                       key, purge_all, expected);
        if (rc != NGX_OK) {
            ngx_slab_free_locked(queue->shpool, item);
            ngx_shmtx_unlock(&queue->shpool->mutex);
            return rc;
        }
    }
    item->path = path;
    item->protection = cmcf->protection;
    item->node.node.key = hash;
    item->purge_all = purge_all;
    item->enqueued_at = ngx_current_msec;
    p = (u_char *) (item + 1);
    if (key_len != 0) {
        ngx_memcpy(p, lookup_key.data, key_len);
    }
    p[key_len] = '\0';
    item->node.str.data = p;
    item->node.str.len = key_len;
    item->pool_name.data = p + key_len + 1;
    item->pool_name.len = cplcf->thread_pool_name.len;
    ngx_memcpy(item->pool_name.data, cplcf->thread_pool_name.data,
               item->pool_name.len);
    ngx_rbtree_insert(&path->rbtree, &item->node.node);
    ngx_http_cache_purge_stat_add(path, NGX_PURGE_ENQUEUED, 1);
    ngx_http_cache_purge_stat_add(path, NGX_PURGE_ENQUEUED_FULL, purge_all);
    if (cplcf->namespace != NULL) {
        ngx_http_cache_purge_stat_add(path, NGX_PURGE_INVALIDATED, 1);
    }
    if (ngx_queue_empty(&queue->pending)) {
        queue->oldest = item->enqueued_at;
    }
    ngx_queue_insert_tail(&queue->pending, &item->link);
    queue->size++;
    ngx_shmtx_unlock(&queue->shpool->mutex);
    return NGX_OK;
}
# endif

static ngx_msec_t
ngx_http_cache_purge_age(ngx_msec_t since)
{
    ngx_msec_int_t  diff;

    diff = (ngx_msec_int_t) (ngx_current_msec - since);

    return (diff < 0) ? 0 : (ngx_msec_t) diff;
}


static void
ngx_http_cache_purge_recover(ngx_http_cache_purge_queue_t *queue)
{
    ngx_http_cache_purge_queue_item_t  *item;
    ngx_uint_t                         n, limit;

    if (!ngx_shmtx_trylock(&queue->shpool->mutex)) {
        return;
    }
    limit = ngx_min(queue->running, NGX_CACHE_PURGE_WALK_ENTRIES);
    for (n = 0; n < limit; n++) {
        item = ngx_queue_data(ngx_queue_head(&queue->active),
                             ngx_http_cache_purge_queue_item_t, link);
        ngx_queue_remove(&item->link);
        if (queue->owners[item->owner_slot] == item->owner
            && queue->epochs[item->owner_slot] == item->owner_epoch
            && ngx_http_cache_purge_process_alive(item->owner))
        {
            ngx_queue_insert_tail(&queue->active, &item->link);
            continue;
        }
        item->running = 0;
        item->owner = 0;
        item->rerun = 0;
        if (item->next_protection != NULL) {
            item->protection = item->next_protection;
            item->next_protection = NULL;
        }
        item->enqueued_at = ngx_current_msec;
        if (ngx_queue_empty(&queue->pending)) {
            queue->oldest = item->enqueued_at;
        }
        ngx_queue_insert_tail(&queue->pending, &item->link);
        queue->running--;
        queue->size++;
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_FINISHED, 1);
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_ENQUEUED, 1);
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_ENQUEUED_FULL,
                                    item->purge_all);
    }
    ngx_shmtx_unlock(&queue->shpool->mutex);
}


/* Move task state with the zone lock; all file work happens after it. */
static ngx_int_t
ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle, ngx_uint_t final)
{
    ngx_http_cache_purge_work_t        *work = &ngx_cache_purge_work;
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *item, *head;
    ngx_int_t                          rc;
    ngx_uint_t                         replay, i, retry, inspected;
    ngx_http_cache_purge_namespace_t  **namespaces;

    queue = work->conf->queue;
    item = work->item;
    if (item == NULL) {
        if (final || ngx_exiting || ngx_terminate || ngx_quit) {
            return NGX_OK;
        }
        if (!ngx_shmtx_trylock(&queue->shpool->mutex)) {
            return NGX_BUSY;
        }
        if (ngx_queue_empty(&queue->pending)) {
            ngx_shmtx_unlock(&queue->shpool->mutex);
            return NGX_OK;
        }
        /* Rotate delayed retries so another path can make progress. The
         * lock covers at most 64 entries, even with a very large queue. */
        for (inspected = 0; inspected < ngx_min(queue->size, 64); inspected++) {
            item = ngx_queue_data(ngx_queue_head(&queue->pending),
                                 ngx_http_cache_purge_queue_item_t, link);
            if (item->retry_at == 0
                || (ngx_msec_int_t) (ngx_current_msec - item->retry_at) >= 0)
            {
                break;
            }
            ngx_queue_remove(&item->link);
            ngx_queue_insert_tail(&queue->pending, &item->link);
        }
        if (inspected == ngx_min(queue->size, 64)) {
            ngx_shmtx_unlock(&queue->shpool->mutex);
            return NGX_BUSY;
        }
        item->retry_at = 0;
        ngx_queue_remove(&item->link);
        ngx_queue_insert_tail(&queue->active, &item->link);
        item->running = 1;
        item->owner = ngx_pid;
        item->owner_slot = ngx_cache_purge_slot;
        item->owner_epoch = queue->epochs[ngx_cache_purge_slot];
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_STARTED, 1);
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_STARTED_FULL,
                                    item->purge_all);
        queue->size--;
        queue->running++;
        if (!ngx_queue_empty(&queue->pending)) {
            head = ngx_queue_data(ngx_queue_head(&queue->pending),
                                 ngx_http_cache_purge_queue_item_t, link);
            queue->oldest = head->enqueued_at;
        }
        ngx_shmtx_unlock(&queue->shpool->mutex);

        work->item = item;
        work->event.cancelable = 0;
        ngx_http_cache_purge_walk_init(&work->ctx, &item->path->name,
                                       item->protection, NULL, cycle->log);
        work->ctx.key_partial = item->node.str.data + 1;
        work->ctx.key_len = item->node.str.len - 1;
        if (work->ctx.key_len != 0
            && work->ctx.key_partial[work->ctx.key_len - 1] == '*')
        {
            work->ctx.key_len--;
        }
        work->cursor.ctx = &work->ctx;
        work->cursor.started = 0;
        work->cursor.complete = 0;
        if (item->node.str.data[0] == 'G') {
            namespaces = work->conf->namespaces.elts;
            for (i = 0; i < work->conf->namespaces.nelts; i++) {
                if (namespaces[i]->name.len == work->ctx.key_len
                    && ngx_memcmp(namespaces[i]->name.data,
                                  work->ctx.key_partial, work->ctx.key_len) == 0)
                {
                    work->ctx.namespace = namespaces[i];
                    break;
                }
            }
            work->ctx.key_len = 0;
            if (work->ctx.namespace == NULL) {
                /* Namespace removed on reload. Its files are unreachable and
                 * core inactive expiry owns their remaining physical cleanup. */
                work->cursor.started = work->cursor.complete = 1;
            }
        }
#if (NGX_THREADS && !NGX_WIN32)
        work->pool = work->task != NULL
                     ? ngx_thread_pool_get(cycle, &item->pool_name) : NULL;
        if (work->task != NULL && work->pool == NULL) {
            ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                          "ngx_cache_purge: task pool \"%V\" no longer "
                          "configured for cache path \"%V\"; "
                          "using incremental execution",
                          &item->pool_name, &item->path->name);
        }
#endif
    }

#if (NGX_THREADS && !NGX_WIN32)
    if (!final && work->pool != NULL && !work->cursor.started) {
        work->finished = 0;
        work->task->event.complete = 0;
        if (ngx_thread_task_post(work->pool, work->task) == NGX_OK) {
            ngx_add_timer(&work->task->event, NGX_CACHE_PURGE_QUEUE_TIMEOUT);
            return NGX_DONE;
        }
        /* Core sets active before cond_signal, but links only after success.
         * On NGX_ERROR no task was queued; retry without running worker I/O. */
        work->task->event.active = 0;
        return NGX_BUSY;
    }
#endif

    if (!final && !work->cursor.complete) {
        rc = ngx_http_cache_purge_walk_step(&work->cursor,
                                           NGX_CACHE_PURGE_WALK_ENTRIES);
        if (rc == NGX_AGAIN) {
            return NGX_BUSY;
        }
        work->cursor.complete = 1;
    }

    /* Fast exit transfers an unfinished cursor, never runs an unbounded walk. */
    if (final) {
        while (work->cursor.top != NULL) {
            ngx_http_cache_purge_walk_close(&work->cursor);
        }
        ngx_shmtx_lock(&queue->shpool->mutex);
    } else if (!ngx_shmtx_trylock(&queue->shpool->mutex)) {
        return NGX_BUSY;
    }

    retry = work->ctx.scan_errors || work->ctx.delete_errors;
    replay = item->rerun || !work->cursor.complete;
    if (replay) {
        item->retries = 0;
        item->retry_at = 0;
    } else if (retry && item->retries < 3) {
        item->retry_at = ngx_current_msec
                         + (item->retries == 0 ? 100
                            : item->retries == 1 ? 1000 : 10000);
        item->retries++;
        replay = 1;
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_RETRIED, 1);
    } else if (retry) {
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_EXHAUSTED, 1);
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "ngx_cache_purge: cleanup of \"%V\" exhausted three "
                      "retries; inspect filesystem errors and retry PURGE",
                      &item->path->name);
    }
    ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_FINISHED, 1);
    ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_DELETED,
                                work->ctx.files_deleted);
    ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_DELETE_ERRORS,
                                work->ctx.delete_errors);
    ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_SCAN_ERRORS,
                                work->ctx.scan_errors);
    ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_DELETED_BYTES,
                                work->ctx.bytes_deleted);
    queue->running--;
    ngx_queue_remove(&item->link);
    if (replay) {
        item->running = 0;
        item->owner = 0;
        item->rerun = 0;
        if (item->next_protection != NULL) {
            item->protection = item->next_protection;
            item->next_protection = NULL;
        }
        item->enqueued_at = ngx_current_msec;
        if (ngx_queue_empty(&queue->pending)) {
            queue->oldest = item->enqueued_at;
        }
        ngx_queue_insert_tail(&queue->pending, &item->link);
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_ENQUEUED, 1);
        ngx_http_cache_purge_stat_add(item->path, NGX_PURGE_ENQUEUED_FULL,
                                    item->purge_all);
        queue->size++;
    } else {
        ngx_rbtree_delete(&item->path->rbtree, &item->node.node);
        ngx_slab_free_locked(queue->shpool, item);
    }
    ngx_shmtx_unlock(&queue->shpool->mutex);
    work->item = NULL;
    work->event.cancelable = 1;
    return NGX_AGAIN;
}


static ngx_http_cache_purge_path_t *
ngx_http_cache_purge_path_get(ngx_http_cache_purge_queue_t *queue,
    ngx_str_t *name)
{
    ngx_http_cache_purge_path_t  *path;
    size_t                        len, align, stride, size;

    len = ngx_http_cache_purge_path_length(name);
    if (len == 0 || len > (size_t) -1 - sizeof(*path) - 1) {
        return NULL;
    }

    for (path = queue->paths; path != NULL; path = path->next) {
        if (path->name.len == len
            && ngx_http_cache_purge_path_equal(path->name.data,
                                              name->data, len))
        {
            return path;
        }
    }

    align = ngx_max(ngx_cacheline_size, 64);
    stride = ngx_align(sizeof(ngx_http_cache_purge_stats_t), align);
    size = sizeof(*path) + len + 1 + align + stride * NGX_MAX_PROCESSES;
    path = ngx_slab_calloc_locked(queue->shpool, size);
    if (path == NULL) {
        return NULL;
    }

    path->name.data = (u_char *) (path + 1);
    path->name.len = len;
    ngx_memcpy(path->name.data, name->data, len);
    path->name.data[len] = '\0';
    path->stats = ngx_align_ptr(path->name.data + len + 1,
                               ngx_max(ngx_cacheline_size, 64));
    path->stats_stride = stride;
    ngx_rbtree_init(&path->rbtree, &path->sentinel,
                   ngx_str_rbtree_insert_value);
    path->next = queue->paths;
    ngx_memory_barrier();
    queue->paths = path;

    return path;
}


static ngx_int_t
ngx_http_cache_purge_bind_queue_status(ngx_http_cache_purge_main_conf_t *cmcf)
{
    ngx_http_cache_purge_loc_conf_t  **locations;
    ngx_path_t                       **paths;
    ngx_uint_t                         i;

    paths = cmcf->paths->elts;
    for (i = 0; i < cmcf->paths->nelts; i++) {
        if (paths[i]->manager != NULL && paths[i]->loader != NULL
            && ngx_http_cache_purge_path_get(cmcf->queue, &paths[i]->name) == NULL)
        {
            return NGX_ERROR;
        }
    }
    locations = cmcf->queue_status_locations.elts;
    for (i = 0; i < cmcf->queue_status_locations.nelts; i++) {
        locations[i]->queue_status_path =
            ngx_http_cache_purge_path_get(cmcf->queue,
                                    &locations[i]->queue_status_cache_path);
        if (locations[i]->queue_status_path == NULL) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

/* Cache paths have managers/loaders; other registered nginx paths are temp. */
static ngx_http_cache_purge_protection_t *
ngx_http_cache_purge_protection_get(ngx_http_cache_purge_queue_t *queue,
    ngx_array_t *paths)
{
    ngx_http_cache_purge_protection_t  *protection;
    ngx_path_t                        **p;
    ngx_uint_t                          i, n, j;
    size_t                              size, len;
    u_char                             *data;

    p = paths->elts;
    n = 0;
    size = sizeof(*protection);

    for (i = 0; i < paths->nelts; i++) {
        if (p[i]->manager != NULL || p[i]->loader != NULL) {
            continue;
        }

        len = ngx_http_cache_purge_path_length(&p[i]->name);
        if (len > (size_t) -1 - sizeof(ngx_str_t) - 1
            || size > (size_t) -1 - sizeof(ngx_str_t) - len - 1)
        {
            return NULL;
        }
        size += sizeof(ngx_str_t) + len + 1;
        n++;
    }

    for (protection = queue->protections; protection != NULL;
         protection = protection->next)
    {
        if (protection->nelts != n) {
            continue;
        }

        j = 0;
        for (i = 0; i < paths->nelts; i++) {
            if (p[i]->manager != NULL || p[i]->loader != NULL) {
                continue;
            }
            len = ngx_http_cache_purge_path_length(&p[i]->name);
            if (protection->names[j].len != len
                || !ngx_http_cache_purge_path_equal(protection->names[j].data,
                                                    p[i]->name.data, len))
            {
                break;
            }
            j++;
        }
        if (j == n) {
            return protection;
        }
    }

    protection = ngx_slab_calloc_locked(queue->shpool, size);
    if (protection == NULL) {
        return NULL;
    }

    protection->nelts = n;
    protection->names = (ngx_str_t *) (protection + 1);
    data = (u_char *) (protection->names + n);
    j = 0;

    for (i = 0; i < paths->nelts; i++) {
        if (p[i]->manager != NULL || p[i]->loader != NULL) {
            continue;
        }
        len = ngx_http_cache_purge_path_length(&p[i]->name);
        protection->names[j].data = data;
        protection->names[j].len = len;
        data = ngx_cpymem(data, p[i]->name.data, len);
        *data++ = '\0';
        j++;
    }

    protection->next = queue->protections;
    queue->protections = protection;

    return protection;
}

ngx_int_t
ngx_http_cache_purge_log(ngx_http_request_t *r)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_path_t       *path;
    ngx_str_t                        *name;
    ngx_uint_t                        status;

    if (r != r->main || r->upstream == NULL || r->cache == NULL
        || r->cache->file_cache == NULL)
    {
        return NGX_OK;
    }
    status = r->upstream->cache_status;
    if (status != NGX_HTTP_CACHE_HIT && status != NGX_HTTP_CACHE_MISS) {
        return NGX_OK;
    }
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    if (cmcf->queue == NULL) {
        return NGX_OK;
    }
    name = &r->cache->file_cache->path->name;
    for (path = cmcf->queue->paths; path != NULL; path = path->next) {
        if (path->name.len == ngx_http_cache_purge_path_length(name)
            && ngx_http_cache_purge_path_equal(path->name.data, name->data,
                                               path->name.len))
        {
            ngx_http_cache_purge_stat_add(path, status == NGX_HTTP_CACHE_HIT
                                              ? NGX_PURGE_HIT : NGX_PURGE_MISS, 1);
            break;
        }
    }
    return NGX_OK;
}


static void
ngx_http_cache_purge_queue_status_snapshot(ngx_http_cache_purge_queue_t *queue,
    ngx_http_cache_purge_path_t *path, ngx_http_cache_purge_queue_stat_t *snap)
{
    ngx_http_cache_purge_stats_t  *stats;
    uint64_t                      sum[NGX_PURGE_METRICS];
    uint64_t                      values[NGX_PURGE_METRICS];
    ngx_uint_t                    slot, field;
#if (NGX_PTR_SIZE == 4)
    ngx_uint_t                    seq, tries;
#endif
    ngx_msec_t                    oldest;

    ngx_memzero(snap, sizeof(*snap));
    snap->queue.size = *(volatile ngx_uint_t *) &queue->size;
    snap->queue.running = *(volatile ngx_uint_t *) &queue->running;
    snap->queue.max_size = queue->max_size;
    snap->queue.full = snap->queue.size + snap->queue.running
                      >= snap->queue.max_size;
    oldest = *(volatile ngx_msec_t *) &queue->oldest;
    snap->queue.wait_time = snap->queue.size ? ngx_http_cache_purge_age(oldest) : 0;
    if (path == NULL) {
        return;
    }
    ngx_memzero(sum, sizeof(sum));
    for (slot = 0; slot < NGX_MAX_PROCESSES; slot++) {
        stats = (ngx_http_cache_purge_stats_t *)
                (path->stats + slot * path->stats_stride);
#if (NGX_PTR_SIZE == 4)
        for (tries = 0; tries < 4; tries++) {
            seq = stats->sequence;
            if (seq & 1) {
                continue;
            }
            ngx_memory_barrier();
#endif
            for (field = 0; field < NGX_PURGE_METRICS; field++) {
                values[field] = stats->value[field];
            }
#if (NGX_PTR_SIZE == 4)
            ngx_memory_barrier();
            if (seq != stats->sequence) {
                continue;
            }
#endif
            for (field = 0; field < NGX_PURGE_METRICS; field++) {
                sum[field] += ngx_min(values[field], (uint64_t) -1 - sum[field]);
            }
#if (NGX_PTR_SIZE == 4)
            break;
        }
#endif
    }
    snap->cache.pending = sum[NGX_PURGE_ENQUEUED]
                          - ngx_min(sum[NGX_PURGE_STARTED], sum[NGX_PURGE_ENQUEUED]);
    snap->cache.pending_full = sum[NGX_PURGE_ENQUEUED_FULL]
        - ngx_min(sum[NGX_PURGE_STARTED_FULL], sum[NGX_PURGE_ENQUEUED_FULL]);
    snap->cache.active = sum[NGX_PURGE_STARTED]
                        - ngx_min(sum[NGX_PURGE_FINISHED], sum[NGX_PURGE_STARTED]);
    snap->cache.overflow = sum[NGX_PURGE_OVERFLOW];
    snap->cache.deleted = sum[NGX_PURGE_DELETED];
    snap->cache.deduplicated = sum[NGX_PURGE_DEDUPLICATED];
    snap->cache.waiting = sum[NGX_PURGE_WAITING];
    snap->cache.errors = sum[NGX_PURGE_DELETE_ERRORS];
    snap->cache.bytes = sum[NGX_PURGE_DELETED_BYTES];
    snap->cache.hits = sum[NGX_PURGE_HIT];
    snap->cache.misses = sum[NGX_PURGE_MISS];
    snap->cache.invalidated = sum[NGX_PURGE_INVALIDATED];
    snap->cache.scan_errors = sum[NGX_PURGE_SCAN_ERRORS];
    snap->cache.retries = sum[NGX_PURGE_RETRIED];
    snap->cache.exhausted = sum[NGX_PURGE_EXHAUSTED];
}


ngx_int_t
ngx_http_cache_purge_queue_status_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_queue_stat_t  snap;
    u_char                             buf[NGX_CACHE_PURGE_STATUS_BODY_MAX];
    u_char                            *p;
    ngx_str_t                          allow = ngx_string("GET, HEAD");
    ngx_buf_t                         *b;
    ngx_chain_t                        out;
    ngx_int_t                          rc;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return ngx_http_cache_purge_method_response(r, &allow);
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);

    if (cmcf->queue != NULL) {
        ngx_http_cache_purge_queue_status_snapshot(cmcf->queue,
                                             cplcf->queue_status_path, &snap);
    } else {
        ngx_memzero(&snap, sizeof(ngx_http_cache_purge_queue_stat_t));
    }

    p = ngx_snprintf(buf, sizeof(buf),
                     "{\"code\":\"status\",\"queue\":{\"size\":%ui,\"running\":%ui,\"max_size\":%ui,"
                     "\"wait_time\":%M,\"full\":%s},"
                     "\"cache\":{\"pending\":{\"total\":%uL,\"full\":%uL},"
                     "\"active\":%uL,\"requests\":{\"overflow\":%uL,"
                     "\"deduplicated\":%uL,\"waiting\":%uL},"
                     "\"files\":{\"deleted\":%uL,\"errors\":%uL,\"bytes\":%uL},"
                     "\"cleanup\":{\"scan_errors\":%uL,\"retries\":%uL,\"exhausted\":%uL},"
                     "\"logical\":{\"invalidations\":%uL},"
                     "\"responses\":{\"hit\":%uL,\"miss\":%uL,\"hit_ratio\":%.6f}}}",
                     snap.queue.size, snap.queue.running, snap.queue.max_size,
                     snap.queue.wait_time,
                     (u_char *) (snap.queue.full ? "true" : "false"),
                     snap.cache.pending, snap.cache.pending_full,
                     snap.cache.active, snap.cache.overflow,
                     snap.cache.deduplicated, snap.cache.waiting,
                     snap.cache.deleted, snap.cache.errors, snap.cache.bytes,
                     snap.cache.scan_errors, snap.cache.retries, snap.cache.exhausted,
                     snap.cache.invalidated,
                     snap.cache.hits, snap.cache.misses,
                     (snap.cache.hits || snap.cache.misses)
                     ? (double) snap.cache.hits
                       / ((double) snap.cache.hits + (double) snap.cache.misses)
                     : 0.0);
    if (p == buf + sizeof(buf)) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }

    /* Allocate before sending headers; the body has to persist. */
    b = NULL;
    if (!r->header_only && r->method != NGX_HTTP_HEAD) {
        b = ngx_create_temp_buf(r->pool, (size_t) (p - buf));
        if (b == NULL) {
            return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
        }
        b->last = ngx_cpymem(b->last, buf, (size_t) (p - buf));
        b->last_buf = (r == r->main);
        b->last_in_chain = 1;
    }

    if (ngx_http_cache_purge_response_headers(r, NGX_PURGE_RESULT_STATUS) != NGX_OK) {
        return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_INTERNAL_ERROR);
    }
    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_length_n = p - buf;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only || b == NULL) {
        return rc;
    }

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


ngx_int_t
ngx_http_cache_purge_probe_credit(ngx_http_cache_purge_queue_t *queue,
    ngx_flag_t take)
{
    ngx_uint_t i, total;

    if (take && !ngx_shmtx_trylock(&queue->shpool->mutex)) {
        return NGX_AGAIN;
    }
    if (!take) {
        ngx_shmtx_lock(&queue->shpool->mutex);
        if (queue->probes[ngx_cache_purge_slot] != 0) {
            queue->probes[ngx_cache_purge_slot]--;
        }
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_OK;
    }
    total = 0;
    for (i = 0; i < NGX_MAX_PROCESSES; i++) {
        if (queue->probes[i] != 0
            && (queue->owners[i] == 0
                || !ngx_http_cache_purge_process_alive(queue->owners[i])))
        {
            queue->probes[i] = 0;
        }
        total += queue->probes[i];
    }
    if (total >= NGX_CACHE_PURGE_REFRESH_PROBES) {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_AGAIN;
    }
    queue->probes[ngx_cache_purge_slot]++;
    ngx_shmtx_unlock(&queue->shpool->mutex);
    return NGX_OK;
}

ngx_flag_t
ngx_http_cache_purge_threaded(void)
{
#if (NGX_THREADS && !NGX_WIN32)
    return ngx_cache_purge_work.task != NULL;
#else
    return 0;
#endif
}

#endif
