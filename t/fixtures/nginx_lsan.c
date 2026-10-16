/*
 * Test-only default for the instrumented NGINX executable. Core allocates
 * connection/read/write-event arrays in ngx_event_process_init() and leaves
 * them to process exit after dropping the cycle pool. These are the three
 * allocations reported on worker TERM after reload; module allocations and
 * general ngx_alloc()/ngx_palloc() callers must remain checked.
 *
 * Upstream report and proposed core fix:
 * https://mailman.nginx.org/pipermail/nginx-devel/2024-February/V5O33KADRTB63SXHS4IWCGIRIIYGN23H.html
 */
const char *
__lsan_default_suppressions(void)
{
    return "leak:^ngx_event_process_init$\n";
}
