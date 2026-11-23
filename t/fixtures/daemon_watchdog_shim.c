#include <assert.h>
#include <stddef.h>
#include <stdint.h>

typedef intptr_t ngx_int_t;
typedef uintptr_t ngx_uint_t;
typedef intptr_t ngx_flag_t;
typedef struct ngx_cycle_s ngx_cycle_t;

typedef struct {
    ngx_uint_t  daemon;
} ngx_core_conf_t;

typedef struct {
    struct {
        ngx_uint_t  nelts;
    } namespaces;
} ngx_http_cache_purge_main_conf_t;

struct ngx_cycle_s {
    void                              *conf_ctx;
    void                              *log;
    ngx_http_cache_purge_main_conf_t  *cmcf;
};

#define NGX_OK     0
#define NGX_ERROR  -1

#define ngx_http_cycle_get_module_main_conf(cycle, module) ((cycle)->cmcf)
#define ngx_get_conf(ctx, module) (ctx)

static ngx_cycle_t *volatile ngx_cache_purge_shutdown_cycle;
static ngx_flag_t ngx_cache_purge_shutdown_started;
static ngx_uint_t watchdog_calls;
static ngx_uint_t shutdown_calls;

static ngx_int_t
ngx_http_cache_purge_generation_boot(ngx_cycle_t *cycle)
{
    (void) cycle;
    return NGX_OK;
}

static void
ngx_http_cache_purge_generation_shutdown(ngx_cycle_t *cycle)
{
    (void) cycle;
    shutdown_calls++;
}

static ngx_int_t
ngx_http_cache_purge_shutdown_watchdog_start(ngx_cycle_t *cycle)
{
    (void) cycle;
    if (ngx_cache_purge_shutdown_started) {
        return NGX_OK;
    }
    ngx_cache_purge_shutdown_started = 1;
    watchdog_calls++;
    return NGX_OK;
}
