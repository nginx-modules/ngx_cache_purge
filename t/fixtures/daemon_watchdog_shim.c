#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int ngx_flag_t;
typedef int ngx_int_t;
typedef unsigned int ngx_uint_t;
typedef unsigned char u_char;
typedef void ngx_log_t;
typedef int pthread_t;
typedef int pthread_attr_t;
typedef int sigset_t;
typedef int pthread_mutex_t;

#define NGX_OK 0
#define NGX_ERROR -1
#define NGX_LOG_ALERT 0
#define ngx_log_error(...) ((void) 0)

typedef struct {
    ngx_uint_t nelts;
} ngx_array_t;

typedef struct {
    ngx_flag_t daemon;
} ngx_core_conf_t;

typedef struct {
    u_char *data;
} ngx_http_cache_purge_traffic_class_conf_t;

typedef struct {
    ngx_array_t namespaces;
    ngx_http_cache_purge_traffic_class_conf_t *classes;
} ngx_http_cache_purge_main_conf_t;

typedef struct {
    void *conf_ctx;
    void *log;
    void *cmcf;
} ngx_cycle_t;

static int ngx_core_module;
static int ngx_http_cache_purge_module;
static ngx_cycle_t *volatile ngx_cache_purge_shutdown_cycle;
static volatile ngx_flag_t ngx_cache_purge_shutdown_stop;
static volatile ngx_flag_t ngx_cache_purge_shutdown_done;
static ngx_flag_t ngx_cache_purge_shutdown_started;
static ngx_flag_t ngx_cache_purge_atfork_restart;
static ngx_uint_t watchdog_calls;
static ngx_uint_t shutdown_calls;
static pthread_mutex_t ngx_cache_purge_fork_mutex;
static char lock_events[32];

static void *
ngx_get_conf(void *conf_ctx, int module)
{
    (void) module;
    return conf_ctx;
}

static ngx_http_cache_purge_main_conf_t *
ngx_http_cycle_get_module_main_conf(ngx_cycle_t *cycle, int module)
{
    (void) module;
    return cycle->cmcf;
}

static void
record_lock_event(char event, ...)
{
    size_t len = strlen(lock_events);
    if (len + 1 < sizeof(lock_events)) {
        lock_events[len] = event;
        lock_events[len + 1] = '\0';
    }
}

static int
pthread_mutex_lock(pthread_mutex_t *mutex)
{
    (void) mutex;
    record_lock_event('L');
    return 0;
}

static int
pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    (void) mutex;
    record_lock_event('U');
    return 0;
}

static ngx_int_t
ngx_http_cache_purge_traffic_class_commit(
    ngx_http_cache_purge_main_conf_t *cmcf, void *log)
{
    (void) cmcf;
    (void) log;
    return NGX_OK;
}

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
    record_lock_event('R');
    watchdog_calls++;
    return NGX_OK;
}

static void
ngx_http_cache_purge_shutdown_watchdog_stop(ngx_cycle_t *cycle)
{
    (void) cycle;
    record_lock_event('S');
    ngx_cache_purge_shutdown_started = 0;
}
