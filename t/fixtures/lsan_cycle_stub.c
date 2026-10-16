/* A cycle allocation plus an unrelated leak in a configuration callback. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <assert.h>

ngx_uint_t  ngx_test_config;
int         lsan_cycle_fail, lsan_cycle_leak;

static void *volatile allocation;

static __attribute__((noinline)) void
ngx_cache_purge_lsan_probe(void)
{
    allocation = malloc(17);
    assert(allocation != NULL);
    allocation = NULL;
}

ngx_cycle_t *
ngx_init_cycle(ngx_cycle_t *old_cycle)
{
    ngx_cycle_t  *cycle;

    assert(old_cycle != NULL && old_cycle->pool != NULL);
    if (lsan_cycle_leak) {
        ngx_cache_purge_lsan_probe();
    }
    if (lsan_cycle_fail) {
        /* Core leaves main()'s bootstrap pool alive on failed validation. */
        return NULL;
    }
    cycle = calloc(1, sizeof(*cycle));
    assert(cycle != NULL);
    cycle->connection_n = 7;
    free(old_cycle->pool);
    return cycle;
}
