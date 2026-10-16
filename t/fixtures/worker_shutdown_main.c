int main(void)
{
    ngx_http_cache_purge_work_t *work = &ngx_cache_purge_work;
    ngx_thread_task_t task = { { 0, 1 } };
    work->task = &task;
    work->event.timer_set = 1;
    work->item = work;
    work->cursor.path.data = work;
    ngx_http_cache_purge_exit_worker(NULL);
    assert(work->event.timer_set == 0);
    assert(finalized == 0 && freed == 0);
    assert(work->item == work && work->cursor.path.data == work);
    work->finished = 1;
    ngx_http_cache_purge_exit_worker(NULL);
    assert(finalized == 1 && freed == 1);
    assert(work->item == NULL && work->cursor.path.data == NULL);
    work->task = NULL;
    work->item = work;
    work->cursor.path.data = work;
    ngx_http_cache_purge_exit_worker(NULL);
    assert(finalized == 2 && freed == 2);
    return 0;
}
