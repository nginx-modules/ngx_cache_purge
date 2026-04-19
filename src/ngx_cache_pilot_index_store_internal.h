#ifndef _NGX_CACHE_PILOT_INDEX_STORE_INTERNAL_H_INCLUDED_
#define _NGX_CACHE_PILOT_INDEX_STORE_INTERNAL_H_INCLUDED_

#include "ngx_cache_pilot_index.h"

#if (NGX_LINUX)

typedef struct ngx_http_cache_index_store_ctx_s
    ngx_http_cache_index_store_ctx_t;
typedef struct ngx_http_cache_index_store_shctx_s
    ngx_http_cache_index_store_shctx_t;
typedef struct ngx_http_cache_index_shm_zone_s
    ngx_http_cache_index_shm_zone_t;
typedef struct ngx_http_cache_index_shm_file_s
    ngx_http_cache_index_shm_file_t;

struct ngx_http_cache_index_store_shctx_s {
    ngx_queue_t                    zones;
    ngx_uint_t                     alloc_failures;
};

struct ngx_http_cache_index_store_ctx_s {
    ngx_slab_pool_t               *shpool;
    ngx_http_cache_index_store_shctx_t *sh;
};

struct ngx_http_cache_index_shm_zone_s {
    ngx_queue_t                    queue;
    ngx_queue_t                    files;
    ngx_flag_t                     bootstrap_complete;
    time_t                         last_bootstrap_at;
    size_t                         name_len;
    u_char                         name[1];
};

struct ngx_http_cache_index_shm_file_s {
    ngx_queue_t                    queue;
    time_t                         mtime;
    off_t                          size;
    ngx_uint_t                     tag_count;
    size_t                         path_len;
    size_t                         key_len;
    u_char                         data[1];
};

typedef struct ngx_http_cache_index_store_ops_s
    ngx_http_cache_index_store_ops_t;

struct ngx_http_cache_index_store_s {
    const ngx_http_cache_index_store_ops_t *ops;
    ngx_http_cache_index_backend_e          backend;
    ngx_flag_t                            readonly;
    union {
        struct {
            ngx_http_cache_index_store_ctx_t *ctx;
            ngx_flag_t                    batch_locked;
        } shm;
    } u;
};

struct ngx_http_cache_index_store_ops_s {
    void (*close)(ngx_http_cache_index_store_t *store);
    ngx_int_t (*begin_batch)(ngx_http_cache_index_store_t *store, ngx_log_t *log);
    ngx_int_t (*commit_batch)(ngx_http_cache_index_store_t *store, ngx_log_t *log);
    ngx_int_t (*rollback_batch)(ngx_http_cache_index_store_t *store, ngx_log_t *log);
    ngx_int_t (*upsert_file_meta)(ngx_http_cache_index_store_t *store,
                                  ngx_str_t *zone_name, ngx_str_t *path,
                                  ngx_str_t *cache_key_text,
                                  time_t mtime, off_t size,
                                  ngx_array_t *tags, ngx_log_t *log);
    ngx_int_t (*delete_file)(ngx_http_cache_index_store_t *store,
                             ngx_str_t *zone_name, ngx_str_t *path, ngx_log_t *log);
    ngx_int_t (*collect_paths_by_tags)(ngx_http_cache_index_store_t *store,
                                       ngx_pool_t *pool, ngx_str_t *zone_name, ngx_array_t *tags,
                                       ngx_array_t **paths, ngx_log_t *log);
    ngx_int_t (*collect_paths_by_exact_key)(ngx_http_cache_index_store_t *store,
                                            ngx_pool_t *pool, ngx_str_t *zone_name,
                                            ngx_str_t *key_text,
                                            ngx_array_t **paths, ngx_log_t *log);
    ngx_int_t (*collect_paths_by_key_prefix)(ngx_http_cache_index_store_t *store,
            ngx_pool_t *pool, ngx_str_t *zone_name,
            ngx_str_t *prefix,
            ngx_array_t **paths, ngx_log_t *log);
    ngx_int_t (*get_zone_state)(ngx_http_cache_index_store_t *store,
                                ngx_str_t *zone_name, ngx_http_cache_index_zone_state_t *state,
                                ngx_log_t *log);
    ngx_int_t (*set_zone_state)(ngx_http_cache_index_store_t *store,
                                ngx_str_t *zone_name, ngx_http_cache_index_zone_state_t *state,
                                ngx_log_t *log);
};

#endif

#endif
