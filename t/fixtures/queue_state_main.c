int main(void)
{
    ngx_http_cache_purge_queue_t queue;
    ngx_http_cache_purge_main_conf_t conf;
    ngx_http_cache_purge_loc_conf_t location;
    ngx_http_cache_purge_work_t *work = &ngx_cache_purge_work;
    ngx_http_cache_purge_queue_item_t *first;
    ngx_http_cache_purge_path_t *path, *next;
    ngx_http_cache_purge_queue_stat_t snap;
    ngx_http_file_cache_t cache;
    ngx_http_request_t request;
    ngx_connection_t connection;
    ngx_cycle_t cycle;
    ngx_slab_pool_t slab;
    ngx_path_t root;
    ngx_str_t a = ngx_string("alpha*"), b = ngx_string("bravo*");
    ngx_str_t other = ngx_string("other*");
    void *main_conf[1], *loc_conf[1];
    unsigned i;
#if (NGX_THREADS)
    ngx_thread_task_t task;
#endif

    memset(&queue, 0, sizeof(queue));
    memset(&conf, 0, sizeof(conf));
    memset(&location, 0, sizeof(location));
    memset(&cache, 0, sizeof(cache));
    memset(&request, 0, sizeof(request));
    memset(&connection, 0, sizeof(connection));
    memset(&cycle, 0, sizeof(cycle));
    memset(&slab, 0, sizeof(slab));
    memset(&root, 0, sizeof(root));
    ngx_queue_init(&queue.pending);
    ngx_queue_init(&queue.active);
    queue.shpool = &slab;
    queue.max_size = 2;
    queue.owners[0] = ngx_pid;
    queue.epochs[0] = 1;
    conf.queue = &queue;
    work->conf = &conf;
    main_conf[0] = &conf;
    loc_conf[0] = &location;
    request.main_conf = main_conf;
    request.loc_conf = loc_conf;
    request.connection = &connection;
    cache.path = &root;
    ngx_str_set(&root.name, "/cache/a");
    ngx_str_set(&location.thread_pool_name, "cleanup");
    test_lock(&slab.mutex);
    path = ngx_http_cache_purge_path_get(&queue, &root.name);
    test_unlock(&slab.mutex);
    assert((uintptr_t) path->stats % 64 == 0 && path->stats_stride % 64 == 0);

    assert(ngx_http_cache_purge_enqueue(&request, &cache, &a, 0, NULL) == NGX_OK);
    first = ngx_queue_data(ngx_queue_head(&queue.pending),
                          ngx_http_cache_purge_queue_item_t, link);
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &b, 0, NULL) == NGX_OK);
    for (i = 0; i < 10000; i++) {
        assert(ngx_http_cache_purge_enqueue(&request, &cache, &a, 0, NULL) == NGX_OK);
    }
    assert(queue.size == 2 && live == 3);
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &other, 0, NULL) == NGX_AGAIN);
    yield_walk = 1;
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_BUSY);
    assert(work->item == first && queue.running == 1 && queue.size == 1);
    assert(ngx_str_rbtree_lookup(&path->rbtree, &first->node.str, 7) == &first->node);
    for (i = 0; i < 10000; i++) {
        assert(ngx_http_cache_purge_enqueue(&request, &cache, &a, 0, NULL) == NGX_OK);
    }
    assert(first->rerun && live == 3 && queue.size == 1);
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &other, 0, NULL) == NGX_AGAIN);
    ngx_http_cache_purge_queue_status_snapshot(&queue, path, &snap);
    assert(!locked && snap.cache.deduplicated == 20000);
    assert(snap.cache.active == 1 && snap.cache.pending == 1 && snap.queue.full);
    yield_walk = 0;
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_AGAIN);
    assert(queue.size == 2 && queue.running == 0 && live == 3);
    assert(ngx_queue_data(ngx_queue_last(&queue.pending),
                         ngx_http_cache_purge_queue_item_t, link) == first);

#if (NGX_THREADS)
    memset(&task, 0, sizeof(task));
    work->task = &task;
    work->pool = (ngx_thread_pool_t *) &queue;
    reject_post = 1;
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_BUSY);
    assert(posts == 1 && !work->cursor.started && !task.event.active);
    reject_post = 2; /* cond_signal fails after setting active, before linking. */
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_BUSY);
    assert(posts == 2 && !work->cursor.started && !task.event.active);
    reject_post = 0;
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_DONE);
    assert(posts == 3 && task.event.active);
    task.event.active = 0;
    work->cursor.started = work->cursor.complete = 1;
    work->pool = NULL;
#endif
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_AGAIN);
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_AGAIN);
    assert(queue.size == 0 && queue.running == 0 && live == 1);
    assert(path->rbtree.root == path->rbtree.sentinel);

    /* Failed passes retry with bounded backoff, without blocking fresh work. */
    scan_errors = 1;
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &a, 0, NULL) == NGX_OK);
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_AGAIN);
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_BUSY);
    scan_errors = 0;
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &b, 0, NULL) == NGX_OK);
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_AGAIN);
    scan_errors = 1;
    for (i = 0; i < 3; i++) {
        first = ngx_queue_data(ngx_queue_head(&queue.pending),
                              ngx_http_cache_purge_queue_item_t, link);
        ngx_current_msec = first->retry_at;
        assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_AGAIN);
    }
    assert(queue.size == 0 && queue.running == 0 && live == 1);
    ngx_http_cache_purge_queue_status_snapshot(&queue, path, &snap);
    assert(snap.cache.scan_errors == 4 && snap.cache.retries == 3
           && snap.cache.exhausted == 1);
    scan_errors = 0;

    /* Fast shutdown hands an incomplete task back without more filesystem work. */
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &a, 0, NULL) == NGX_OK);
    yield_walk = 1;
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_BUSY);
    assert(ngx_http_cache_purge_process_queue(&cycle, 1) == NGX_AGAIN);
    assert(queue.size == 1 && queue.running == 0);
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_BUSY);
    work->item = NULL; /* lost process-local state after a simulated crash */
    dead_owner = 1;
    ngx_http_cache_purge_recover(&queue);
    assert(queue.running == 0 && queue.size == 1);
    dead_owner = 0;
    yield_walk = 0;
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_AGAIN);

    /* Sequence sampling never waits for a writer; byte counters exceed 32 bits. */
    ngx_http_cache_purge_stat_add(path, NGX_PURGE_DELETED_BYTES,
                                ((uint64_t) 1 << 32) + 3);
    ngx_http_cache_purge_queue_status_snapshot(&queue, path, &snap);
    assert(snap.cache.bytes == ((uint64_t) 1 << 32) + 3);
    assert(snap.cache.active == 0 && snap.cache.pending == 0);
    ngx_http_cache_purge_stats(path)->sequence++;
    ngx_http_cache_purge_queue_status_snapshot(&queue, path, &snap);
#if (NGX_PTR_SIZE == 4)
    assert(snap.cache.bytes == 0 && !locked);
#else
    assert(snap.cache.bytes == ((uint64_t) 1 << 32) + 3 && !locked);
#endif
    ngx_http_cache_purge_stats(path)->sequence++;

    fail_alloc = 1;
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &a, 0, NULL) == NGX_AGAIN);
    assert(queue.size == 0 && path->rbtree.root == path->rbtree.sentinel);
    ngx_quit = 1;
    assert(ngx_http_cache_purge_enqueue(&request, &cache, &a, 0, NULL) == NGX_ABORT);
    ngx_quit = 0;
    assert(ngx_http_cache_purge_process_queue(&cycle, 0) == NGX_OK);
    assert(!locked && ngx_queue_empty(&queue.pending) && ngx_queue_empty(&queue.active));
    for (path = queue.paths; path != NULL; path = next) {
        next = path->next;
        test_lock(&slab.mutex);
        test_free(&slab, path);
        test_unlock(&slab.mutex);
    }
    assert(live == 0);
    return 0;
}
