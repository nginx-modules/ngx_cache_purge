#ifndef _NGX_CACHE_PILOT_METRICS_H_INCLUDED_
#define _NGX_CACHE_PILOT_METRICS_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "ngx_cache_pilot_index.h"

/*
 * Global purge operation counters in shared memory.
 * All fields are ngx_atomic_t — no mutex needed for reads or increments.
 *
 * The struct tag matches the forward declaration in ngx_cache_pilot_index.h.
 */
struct ngx_http_cache_pilot_metrics_shctx_s {
    ngx_atomic_t  purges_exact_hard;
    ngx_atomic_t  purges_exact_soft;
    ngx_atomic_t  purges_wildcard_hard;
    ngx_atomic_t  purges_wildcard_soft;
    ngx_atomic_t  purges_tag_hard;
    ngx_atomic_t  purges_tag_soft;
    ngx_atomic_t  purges_all_hard;
    ngx_atomic_t  purges_all_soft;
    ngx_atomic_t  key_index_exact_fanout;
    ngx_atomic_t  key_index_wildcard_hits;
};

/* Increment one field in the metrics shctx (no-op when metrics == NULL). */
#define NGX_CACHE_PILOT_METRICS_INC(metrics, field)             \
    do {                                                        \
        if ((metrics) != NULL) {                               \
            ngx_atomic_fetch_add(&(metrics)->field, 1);        \
        }                                                      \
    } while (0)

/* Response format identifiers */
#define NGX_CACHE_PILOT_METRICS_FORMAT_JSON        0
#define NGX_CACHE_PILOT_METRICS_FORMAT_PROMETHEUS  1

/* Public API */
char      *ngx_http_cache_pilot_stats_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_cache_pilot_metrics_init_zone(ngx_shm_zone_t *shm_zone,
        void *data);
ngx_int_t  ngx_http_cache_pilot_metrics_init_conf(ngx_conf_t *cf,
        ngx_http_cache_pilot_main_conf_t *pmcf);
ngx_int_t  ngx_http_cache_pilot_metrics_handler(ngx_http_request_t *r);

#endif /* _NGX_CACHE_PILOT_METRICS_H_INCLUDED_ */
