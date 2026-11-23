int
main(void)
{
    ngx_cycle_t                       cycle, reload;
    ngx_core_conf_t                   ccf;
    ngx_http_cache_purge_main_conf_t  cmcf;

    cycle.conf_ctx = &ccf;
    cycle.log = NULL;
    cycle.cmcf = &cmcf;
    cmcf.namespaces.nelts = 1;

    ccf.daemon = 1;
    watchdog_calls = 0;
    shutdown_calls = 0;
    assert(ngx_http_cache_purge_init_module(&cycle) == NGX_OK);
    assert(watchdog_calls == 0);
    assert(shutdown_calls == 0);

    ccf.daemon = 0;
    assert(ngx_http_cache_purge_init_module(&cycle) == NGX_OK);
    assert(watchdog_calls == 1);
    assert(ngx_cache_purge_shutdown_cycle == &cycle);
    assert(shutdown_calls == 0);

    reload.conf_ctx = &ccf;
    reload.log = NULL;
    reload.cmcf = &cmcf;
    ccf.daemon = 1;
    assert(ngx_http_cache_purge_init_module(&reload) == NGX_OK);
    assert(watchdog_calls == 1);
    assert(ngx_cache_purge_shutdown_cycle == &reload);
    assert(shutdown_calls == 0);

    cmcf.namespaces.nelts = 0;
    assert(ngx_http_cache_purge_init_module(&cycle) == NGX_OK);
    assert(watchdog_calls == 1);
    assert(shutdown_calls == 0);

    return 0;
}
