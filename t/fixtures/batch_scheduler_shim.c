#include <assert.h>
#include <stddef.h>

#define NGX_OK 0
#define NGX_AGAIN -2
#define NGX_BUSY -3
#define NGX_DONE -4
#define NGX_CACHE_PURGE_WALK_MSEC 5
typedef int ngx_int_t;
typedef unsigned ngx_uint_t;
typedef unsigned long ngx_msec_t;
typedef long ngx_msec_int_t;
typedef int ngx_cycle_t;
typedef struct { void *data; } ngx_event_t;
typedef struct { ngx_uint_t batch_size; ngx_msec_t throttle_ms; void *queue; } test_conf_t;
typedef struct {
    test_conf_t *conf;
    void *item;
    ngx_uint_t batch;
    ngx_msec_t recover_at;
} ngx_http_cache_purge_work_t;
static ngx_http_cache_purge_work_t ngx_cache_purge_work;
static ngx_msec_t ngx_current_msec, delay, tick;
static ngx_uint_t ngx_exiting, ngx_terminate, ngx_quit, timers, calls;
static ngx_int_t results[8];
static ngx_uint_t position, length;

static ngx_int_t
ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle, ngx_uint_t final)
{
    ngx_int_t rc;

    assert(cycle != NULL && final == 0 && position < length);
    calls++;
    rc = results[position++];
    ngx_cache_purge_work.item = (rc == NGX_DONE || rc == NGX_BUSY)
                               ? &ngx_cache_purge_work : NULL;
    return rc;
}

static void ngx_time_update(void) { ngx_current_msec += tick; }
static void ngx_add_timer(ngx_event_t *ev, ngx_msec_t value)
{
    assert(ev != NULL);
    timers++;
    delay = value;
}

static void script(ngx_int_t first, ngx_int_t second)
{
    results[0] = first;
    results[1] = second;
    length = 2;
    position = 0;
    timers = calls = 0;
}

static ngx_msec_t ngx_http_cache_purge_age(ngx_msec_t since)
{ return ngx_current_msec - since; }
static void ngx_http_cache_purge_recover(void *queue) { (void) queue; }
