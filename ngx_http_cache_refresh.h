#ifndef _NGX_HTTP_CACHE_REFRESH_H_INCLUDED_
#define _NGX_HTTP_CACHE_REFRESH_H_INCLUDED_

#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define NGX_CACHE_PURGE_RESPONSE_TYPE_HTML  1
#define NGX_CACHE_PURGE_RESPONSE_TYPE_XML   2
#define NGX_CACHE_PURGE_RESPONSE_TYPE_JSON  3
#define NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT  4

#define NGX_CACHE_PURGE_KEY_MAX_LEN          512

typedef struct ngx_http_cache_purge_queue_item_s ngx_http_cache_purge_queue_item_t;
typedef struct ngx_http_cache_purge_queue_s ngx_http_cache_purge_queue_t;
typedef struct ngx_http_cache_purge_main_conf_s ngx_http_cache_purge_main_conf_t;

struct ngx_http_cache_purge_queue_item_s {
    ngx_str_t                          cache_path;
    ngx_str_t                          key_partial;
    ngx_uint_t                         hash;
    ngx_flag_t                         purge_all;
    ngx_uint_t                         in_progress;
    ngx_msec_t                         enqueued_at;
    ngx_http_cache_purge_queue_item_t *next;
};

struct ngx_http_cache_purge_queue_s {
    ngx_http_cache_purge_queue_item_t *head;
    ngx_http_cache_purge_queue_item_t *tail;
    ngx_atomic_t                       size;
    ngx_shmtx_sh_t                     sh;
    ngx_shmtx_t                        mutex;
    ngx_slab_pool_t                   *shpool;
    ngx_uint_t                         max_size;
    ngx_uint_t                         batch_size;
    ngx_msec_t                         throttle_ms;
};

struct ngx_http_cache_purge_main_conf_s {
    ngx_http_cache_purge_queue_t      *queue;
    ngx_shm_zone_t                    *shm_zone;
    ngx_uint_t                         queue_size;
    ngx_uint_t                         batch_size;
    ngx_msec_t                         throttle_ms;
    ngx_flag_t                         background_purge;
    ngx_flag_t                         legacy_status_codes;
    ngx_flag_t                         vary_aware;
};

typedef struct {
    ngx_flag_t                    enable;
    ngx_str_t                     method;
    ngx_flag_t                    purge_all;
    ngx_flag_t                    refresh;
    ngx_array_t                  *access;
    ngx_array_t                  *access6;
} ngx_http_cache_purge_conf_t;

typedef struct {
# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_conf_t  fastcgi;
# endif
# if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_conf_t  proxy;
# endif
# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_conf_t  scgi;
# endif
# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_conf_t  uwsgi;
# endif

    ngx_http_cache_purge_conf_t *conf;
    ngx_http_handler_pt          handler;
    ngx_http_handler_pt          original_handler;
    ngx_uint_t                   response_type;

# if (NGX_HTTP_PROXY)
    ngx_shm_zone_t              *proxy_separate_zone;
    ngx_http_complex_value_t    *proxy_separate_value;
    ngx_http_complex_value_t     proxy_separate_key;
# endif
    ngx_uint_t                   refresh_concurrency;
    ngx_msec_t                   refresh_timeout;
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    u_char     *key_partial;
    ngx_uint_t  key_len;
    u_char      key_buffer[512];
    ngx_uint_t  files_deleted;
    ngx_uint_t  files_checked;
} ngx_http_cache_purge_walk_ctx_t;

typedef enum {
    NGX_HTTP_CACHE_PURGE_INVALIDATE_PURGED = 0,
    NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_MISSING,
    NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_REPLACED,
    NGX_HTTP_CACHE_PURGE_INVALIDATE_ERROR
} ngx_http_cache_purge_invalidate_result_e;

typedef struct {
    ngx_str_t        cache_key;
    ngx_str_t        cache_path;
    ngx_file_uniq_t  uniq;
    time_t           last_modified;
    u_short          body_start;
    u_char           etag_len;
    u_char           etag[NGX_HTTP_CACHE_ETAG_LEN];
    off_t            fs_size;
} ngx_http_cache_purge_invalidate_item_t;

typedef struct {
    ngx_str_t          cache_key;
    ngx_str_t          cache_path;
    ngx_http_request_t *request;
    ngx_http_file_cache_t *cache;
    ngx_str_t          partial_prefix;
    ngx_flag_t         match_all;
    ngx_uint_t         files_deleted;
    ngx_queue_t        temp_pools;
} ngx_http_cache_purge_batch_ctx_t;

typedef struct {
    ngx_str_t                         uri;
    ngx_str_t                         args;
    ngx_str_t                         etag;
    time_t                            last_modified;
    ngx_str_t                         path;
    ngx_http_cache_purge_invalidate_item_t item;
} ngx_http_cache_purge_refresh_file_t;

typedef struct {
    ngx_str_t     path;
    ngx_flag_t    is_dir;
} ngx_http_cache_purge_refresh_scan_entry_t;

typedef struct {
    ngx_queue_t   queue;
    ngx_pool_t   *pool;
} ngx_http_cache_purge_refresh_temp_pool_t;

typedef struct {
    ngx_queue_t   queue;
    ngx_str_t     path;
} ngx_http_cache_purge_refresh_dir_t;

typedef struct {
    ngx_uint_t    status;
    ngx_uint_t    count;
    off_t         bytes;
} ngx_http_cache_purge_refresh_status_count_t;

typedef struct {
    ngx_http_request_t              *request;
    ngx_http_file_cache_t           *cache;
    ngx_str_t                        key_partial;
    ngx_uint_t                       key_prefix_len;
    ngx_flag_t                       purge_all;
    ngx_flag_t                       exact;
    ngx_flag_t                       timed_out;
    ngx_flag_t                       timeout_enabled;
    ngx_flag_t                       finalized;
    ngx_msec_t                       deadline;
    ngx_event_t                      timeout_ev;
    ngx_pool_t                      *chunk_pool;
    ngx_pool_t                      *retired_chunk_pool;
    ngx_queue_t                      retired_chunk_pools;
    ngx_pool_t                      *scan_pool;
    ngx_array_t                     *files;
    ngx_array_t                     *scan_entries;
    ngx_uint_t                       current;
    ngx_uint_t                       queued;
    ngx_uint_t                       active;
    ngx_uint_t                       dispatched;
    ngx_uint_t                       chunk_limit;
    ngx_uint_t                       scan_index;
    ngx_flag_t                       scan_done;
    ngx_flag_t                       scan_initialized;
    ngx_queue_t                      temp_pools;
    ngx_queue_t                      pending_dirs;
    ngx_array_t                     *status_counts;
    ngx_uint_t                       total;
    ngx_uint_t                       refreshed;
    ngx_uint_t                       purged;
    ngx_uint_t                       errors;
    off_t                            total_bytes;
    off_t                            kept_bytes;
    off_t                            purged_bytes;
} ngx_http_cache_purge_refresh_ctx_t;

typedef struct {
    ngx_http_cache_purge_refresh_ctx_t  *ctx;
    ngx_http_cache_purge_refresh_file_t *file;
    ngx_flag_t                           validation_ready;
    ngx_flag_t                           handled;
} ngx_http_cache_purge_refresh_post_data_t;

ngx_int_t ngx_http_cache_purge_add_variable(ngx_conf_t *cf);
ngx_int_t ngx_http_cache_purge_refresh(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache);
ngx_int_t ngx_http_cache_purge_add_action_header(ngx_http_request_t *r,
    ngx_uint_t refresh);
void ngx_http_cache_purge_drain_temp_pools(ngx_queue_t *queue);
ngx_int_t ngx_http_cache_purge_invalidate_item(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_pool_t *pool,
    ngx_http_cache_purge_invalidate_item_t *item,
    ngx_http_cache_purge_invalidate_result_e *result);
ngx_int_t ngx_http_cache_purge_is_partial(ngx_http_request_t *r);

extern ngx_module_t ngx_http_cache_purge_module;

#endif /* _NGX_HTTP_CACHE_REFRESH_H_INCLUDED_ */
