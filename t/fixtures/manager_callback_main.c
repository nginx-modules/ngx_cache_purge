/* A prototype of the native cache-manager integration seam. No request or
 * worker state may be used here: Windows runs callbacks in a manager thread. */
#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <assert.h>

#if (nginx_version >= 1011005)
typedef ngx_msec_t manager_interval_t;
#define INTERVAL_MSEC 1
#else
typedef time_t manager_interval_t;
#define INTERVAL_MSEC 1000
#endif

typedef struct {
    ngx_path_manager_pt original_manager;
    ngx_path_loader_pt original_loader;
    void *original_data;
    ngx_msec_t native_due, now;
    unsigned pending, processed;
} adapter_t;

static unsigned evictions, loads;
static int cache_data;
static manager_interval_t native_manager(void *data)
{ assert(data == &cache_data); evictions++; return 1000 / INTERVAL_MSEC; }
static void native_loader(void *data)
{ assert(data == &cache_data); loads++; }

static manager_interval_t adapter_manager(void *data)
{
    adapter_t *adapter = data;
    ngx_msec_t wait;
    unsigned n;

    if (adapter->now >= adapter->native_due) {
        adapter->native_due = adapter->now
            + adapter->original_manager(adapter->original_data) * INTERVAL_MSEC;
    }
    /* Every inspected entry costs budget, including nonmatches. */
    for (n = 0; n < 64 && adapter->pending != 0; n++) {
        adapter->pending--;
        adapter->processed++;
    }
    wait = adapter->native_due > adapter->now ? adapter->native_due - adapter->now : 1;
    if (adapter->pending != 0 && wait > 10) { wait = 10; }
    /* Old core dispatchers wait at least a second even for return value 0. */
    return (wait + INTERVAL_MSEC - 1) / INTERVAL_MSEC;
}

static void adapter_loader(void *data)
{
    adapter_t *adapter = data;
    adapter->original_loader(adapter->original_data);
}

int main(void)
{
    ngx_path_t path;
    adapter_t adapter;
    manager_interval_t wait;

    ngx_memzero(&path, sizeof(path));
    ngx_memzero(&adapter, sizeof(adapter));
    path.manager = native_manager;
    path.loader = native_loader;
    path.data = &cache_data;
    adapter.original_manager = path.manager;
    adapter.original_loader = path.loader;
    adapter.original_data = path.data;
    /* Changing path.data requires wrapping the loader too. */
    path.manager = adapter_manager;
    path.loader = adapter_loader;
    path.data = &adapter;
    adapter.pending = 129;
    path.loader(path.data);
    assert(loads == 1);
    wait = path.manager(path.data);
    assert(evictions == 1 && adapter.processed == 64);
    assert(wait == (10 + INTERVAL_MSEC - 1) / INTERVAL_MSEC);
    adapter.now += wait * INTERVAL_MSEC;
    wait = path.manager(path.data);
    assert(adapter.processed == 128);
    adapter.now += wait * INTERVAL_MSEC;
    (void) path.manager(path.data);
    assert(adapter.processed == 129 && adapter.pending == 0);
    adapter.now = 3000;
    (void) path.manager(path.data);
    assert(evictions >= 2); /* purge must not starve native eviction */
    return 0;
}
