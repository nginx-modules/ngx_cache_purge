#ifndef _NGX_CACHE_PURGE_INVALIDATION_H_INCLUDED_
#define _NGX_CACHE_PURGE_INVALIDATION_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* Generation state is separate from the task deduplication rbtree. */
typedef struct ngx_http_cache_purge_generation_s
    ngx_http_cache_purge_generation_t;

typedef struct {
    ngx_str_t                           name;
    ngx_shm_zone_t                     *zone;
    ngx_path_t                         *state_path;
    ngx_pool_t                         *pool;
    ngx_http_cache_purge_generation_t  *shared;
} ngx_http_cache_purge_namespace_t;

#define NGX_CACHE_PURGE_LOGICAL_KEY_MAX  8096

ngx_http_cache_purge_namespace_t *ngx_http_cache_purge_namespace_create(
    ngx_conf_t *cf, ngx_str_t *name, size_t size, const ngx_str_t *state_path);
ngx_int_t ngx_http_cache_purge_generation_key(ngx_http_request_t *r,
    ngx_http_cache_purge_namespace_t *ns, ngx_http_complex_value_t *expression,
    ngx_str_t *logical, ngx_str_t *encoded);
ngx_int_t ngx_http_cache_purge_generation_invalidate(
    ngx_http_cache_purge_namespace_t *ns, ngx_str_t *key, ngx_flag_t all, ngx_str_t *expected);
ngx_int_t ngx_http_cache_purge_generation_stale(
    ngx_http_cache_purge_namespace_t *ns, ngx_str_t *key);

ngx_int_t ngx_http_cache_purge_generation_current(
    ngx_http_cache_purge_namespace_t *ns, ngx_str_t *key, ngx_str_t *logical);

/* Consistent copy of the namespace generation state, for status reporting. */
typedef struct {
    u_char      epoch[10];
    uint64_t    serial;
    uint64_t    baseline;
    ngx_flag_t  rules;
} ngx_http_cache_purge_generation_view_t;

void ngx_http_cache_purge_generation_view(
    ngx_http_cache_purge_namespace_t *ns,
    ngx_http_cache_purge_generation_view_t *view);

#endif
