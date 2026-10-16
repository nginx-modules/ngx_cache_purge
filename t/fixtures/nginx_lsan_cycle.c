/*
 * Test-only GNU linker wrapper. On successful nginx -t, core returns from
 * main() before assigning the new cycle to ngx_cycle. The configuration pool
 * then loses its last live root at process exit. Keep that process-lifetime
 * configuration reachable, just as core does during a normal server run.
 * On rejected initial configuration, main() instead leaves its 1024-byte
 * bootstrap pool allocated. Retain that pool, not the failed cycle or the
 * stack-allocated old_cycle; core has already destroyed the failed pool.
 *
 * Do not suppress ngx_init_cycle allocation stacks: they also contain module
 * configuration callbacks and would hide unrelated leaks. Failed-cycle
 * allocations and normal server cycles are deliberately left untouched.
 */
#include <ngx_config.h>
#include <ngx_core.h>

static ngx_cycle_t *volatile test_cycle;
static ngx_pool_t  *volatile test_bootstrap_pool;

ngx_cycle_t *__real_ngx_init_cycle(ngx_cycle_t *old_cycle);
ngx_cycle_t *__wrap_ngx_init_cycle(ngx_cycle_t *old_cycle);

ngx_cycle_t *
__wrap_ngx_init_cycle(ngx_cycle_t *old_cycle)
{
    ngx_cycle_t  *cycle;

    cycle = __real_ngx_init_cycle(old_cycle);
    if (ngx_test_config) {
        if (cycle != NULL) {
            test_cycle = cycle;
        } else if (old_cycle != NULL && ngx_is_init_cycle(old_cycle)) {
            test_bootstrap_pool = old_cycle->pool;
        }
    }

    return cycle;
}
