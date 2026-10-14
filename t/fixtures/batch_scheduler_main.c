int main(void)
{
    test_conf_t conf = { 2, 100, NULL };
    ngx_cycle_t cycle = 0;
    ngx_event_t event = { &cycle };
    ngx_http_cache_purge_work_t *work = &ngx_cache_purge_work;

    work->conf = &conf;
    script(NGX_AGAIN, NGX_BUSY);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 2 && work->batch == 1 && delay == 1 && timers == 1);

    script(NGX_BUSY, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 1 && work->batch == 1 && delay == 1 && timers == 1);

    script(NGX_AGAIN, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 1 && work->batch == 0 && delay == 100 && timers == 1);

    script(NGX_AGAIN, NGX_DONE);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 2 && work->batch == 1 && timers == 0);

    script(NGX_AGAIN, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 1 && work->batch == 0 && delay == 100 && timers == 1);

    tick = 5;
    script(NGX_AGAIN, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 1 && work->batch == 1 && delay == 1 && timers == 1);

    script(NGX_AGAIN, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 1 && work->batch == 0 && delay == 100 && timers == 1);

    tick = 0;
    script(NGX_AGAIN, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 2 && work->batch == 0 && delay == 1000 && timers == 1);

    conf.batch_size = 1;
    script(NGX_AGAIN, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 1 && work->batch == 0 && delay == 100 && timers == 1);

    ngx_exiting = 1;
    script(NGX_OK, NGX_OK);
    ngx_http_cache_purge_background_handler(&event);
    assert(calls == 1 && work->batch == 0 && timers == 0);
    return 0;
}
