int main(void)
{
    ngx_http_cache_refresh_t job;
    test_scan_t scan;
    test_upstream_t upstream;
    ngx_http_request_t sr;
    unsigned i;
    ngx_uint_t statuses[] = {304, 200, 404, 410, 500, 0};
    ngx_uint_t outcomes[] = {NGX_REFRESH_KEPT, NGX_REFRESH_PENDING,
        NGX_REFRESH_PENDING, NGX_REFRESH_PENDING, NGX_REFRESH_STATUS,
        NGX_REFRESH_TRANSPORT};

    memset(&sr, 0, sizeof(sr));
    for (i = 0; i < 7; i++) {
        memset(&job, 0, sizeof(job));
        memset(&scan, 0, sizeof(scan));
        job.scan = &scan;
        job.active = 1;
        sr.upstream = i < 6 ? &upstream : NULL;
        upstream.headers_in.status_n = i < 6 ? statuses[i] : 0;
        if (i == 6) { scan.entries[0].result = NGX_REFRESH_PROFILE; }
        assert(ngx_http_cache_refresh_done(&sr, &job,
                    i == 5 ? NGX_ERROR : i == 6 ? 500 : 0) == NGX_OK);
        assert(!job.active && released == i + 1 && scheduled == i + 1);
        assert(job.decision == (i >= 1 && i <= 3));
        assert(scan.entries[0].result == (i < 6 ? outcomes[i] : NGX_REFRESH_PROFILE));
        /* Core may finalize again after a filter flush: no duplicate credit. */
        assert(ngx_http_cache_refresh_done(&sr, &job, 0) == NGX_OK);
        assert(released == i + 1 && scheduled == i + 1);
    }
    return 0;
}
