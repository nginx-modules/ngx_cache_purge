#include <assert.h>
#include <stddef.h>
#include <string.h>

typedef long ngx_int_t;
typedef unsigned long ngx_uint_t;
#define NGX_OK 0
#define NGX_ERROR -1
typedef struct { unsigned timer_set; } ngx_event_t;
typedef struct { ngx_uint_t status, result; } ngx_http_cache_refresh_entry_t;
typedef struct { ngx_http_cache_refresh_entry_t entries[1]; } test_scan_t;
typedef struct {
    struct { ngx_uint_t status_n; } headers_in;
} test_upstream_t;
typedef struct { test_upstream_t *upstream; } ngx_http_request_t;
typedef struct {
    unsigned active, decision;
    ngx_uint_t position;
    test_scan_t *scan;
    ngx_event_t event;
} ngx_http_cache_refresh_t;
static unsigned released, scheduled;
static ngx_int_t ngx_http_cache_refresh_credit(ngx_http_cache_refresh_t *job,
    unsigned take)
{ (void) job; assert(!take); released++; return NGX_OK; }
static void ngx_add_timer(ngx_event_t *ev, unsigned delay)
{ assert(delay == 1); ev->timer_set = 1; scheduled++; }
