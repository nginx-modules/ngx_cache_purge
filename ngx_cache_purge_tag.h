#ifndef _NGX_CACHE_PURGE_TAG_H_INCLUDED_
#define _NGX_CACHE_PURGE_TAG_H_INCLUDED_

#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_LINUX)
#include <sys/inotify.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#endif

typedef struct ngx_http_cache_tag_zone_s
    ngx_http_cache_tag_zone_t;
typedef struct ngx_http_cache_tag_store_s
    ngx_http_cache_tag_store_t;

typedef struct {
    ngx_flag_t                    enable;
    ngx_flag_t                    configured;
    ngx_array_t                  *enable_values;
    ngx_flag_t                    soft;
    ngx_flag_t                    purge_all;
} ngx_http_cache_purge_conf_t;

typedef struct {
#if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_conf_t   fastcgi;
#endif
#if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_conf_t   proxy;
#endif
#if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_conf_t   scgi;
#endif
#if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_conf_t   uwsgi;
#endif

    ngx_http_cache_purge_conf_t  *conf;
    ngx_http_handler_pt           handler;
    ngx_http_handler_pt           original_handler;

    ngx_uint_t                    resptype;
    ngx_flag_t                    cache_tag_watch;
    ngx_array_t                  *cache_tag_headers;
    ngx_str_t                     purge_mode_header;

    /* cache_purge_stats: zones registered for the metrics endpoint */
    ngx_array_t                  *stat_zones;  /* ngx_http_cache_purge_stat_zone_t */
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    ngx_str_t                     endpoint;
    ngx_str_t                     host;
    ngx_str_t                     unix_path;
    ngx_str_t                     password;
    ngx_uint_t                    port;
    ngx_uint_t                    db;
    ngx_flag_t                    use_unix;
    /* TCP address resolved once at config-parse time so that reconnects
     * inside worker processes never call getaddrinfo() in the event loop. */
    struct sockaddr_storage       resolved_addr;
    socklen_t                     resolved_addrlen;
    ngx_flag_t                    resolved;
} ngx_http_cache_tag_redis_conf_t;

typedef enum {
    NGX_HTTP_CACHE_TAG_BACKEND_NONE = 0,
    NGX_HTTP_CACHE_TAG_BACKEND_SQLITE,
    NGX_HTTP_CACHE_TAG_BACKEND_REDIS
} ngx_http_cache_tag_backend_e;

/*
 * Forward declaration for ngx_http_cache_purge_metrics_shctx_t.
 * Full definition is in ngx_cache_purge_metrics.h.
 */
struct ngx_http_cache_purge_metrics_shctx_s;
typedef struct ngx_http_cache_purge_metrics_shctx_s
    ngx_http_cache_purge_metrics_shctx_t;

/*
 * One cache zone registered for the stats endpoint.
 * Populated at config time from the upstream-module main confs.
 */
typedef struct {
    ngx_str_t              name;
    ngx_http_file_cache_t *cache;
} ngx_http_cache_purge_stat_zone_t;

typedef struct {
    ngx_http_cache_tag_backend_e           backend;
    ngx_str_t                              sqlite_path;
    ngx_http_cache_tag_redis_conf_t        redis;
    ngx_array_t                           *zones;
    size_t                                 queue_shm_size;
#if (NGX_LINUX)
    ngx_shm_zone_t                        *queue_zone;
#endif
    /* Metrics shared-memory zone and pointer (set in init_process) */
    ngx_shm_zone_t                        *metrics_zone;
    ngx_http_cache_purge_metrics_shctx_t  *metrics;
} ngx_http_cache_purge_main_conf_t;

struct ngx_http_cache_tag_zone_s {
    ngx_str_t                     zone_name;
    ngx_http_file_cache_t        *cache;
    ngx_array_t                  *headers;
};

#if (NGX_LINUX)

typedef struct {
    ngx_flag_t                    bootstrap_complete;
    time_t                        last_bootstrap_at;
} ngx_http_cache_tag_zone_state_t;

typedef struct {
    ngx_rbtree_node_t             node;
    ngx_http_cache_tag_zone_t    *zone;
} ngx_http_cache_tag_zone_index_t;

typedef struct ngx_http_cache_tag_watch_s {
    ngx_rbtree_node_t             node;
    ngx_str_t                     zone_name;
    ngx_http_file_cache_t        *cache;
    ngx_str_t                     path;
    int                           wd;
} ngx_http_cache_tag_watch_t;

typedef struct {
    ngx_uint_t                    operation;
    ngx_str_t                     zone_name;
    ngx_http_file_cache_t        *cache;
    ngx_str_t                     path;
} ngx_http_cache_tag_pending_op_t;

typedef struct {
    ngx_uint_t                    accepted;
    ngx_uint_t                    rejected;
    ngx_uint_t                    drained;
} ngx_http_cache_tag_queue_stats_t;

#define NGX_HTTP_CACHE_TAG_QUEUE_ZONE_NAME_MAX  256
#define NGX_HTTP_CACHE_TAG_QUEUE_PATH_MAX       4096
#define NGX_HTTP_CACHE_TAG_QUEUE_CAPACITY       256
#define NGX_HTTP_CACHE_TAG_QUEUE_SIZE           (2 * 1024 * 1024)

#define NGX_HTTP_CACHE_TAG_MAX_TAGS_PER_FILE    1000

typedef struct {
    ngx_uint_t                    operation;
    size_t                        zone_name_len;
    size_t                        path_len;
    u_char                        zone_name[NGX_HTTP_CACHE_TAG_QUEUE_ZONE_NAME_MAX];
    u_char                        path[NGX_HTTP_CACHE_TAG_QUEUE_PATH_MAX];
} ngx_http_cache_tag_queue_entry_t;

typedef struct {
    ngx_uint_t                    head;
    ngx_uint_t                    tail;
    ngx_uint_t                    count;
    ngx_uint_t                    capacity;
    ngx_uint_t                    dropped;
    ngx_http_cache_tag_queue_entry_t entries[1];
} ngx_http_cache_tag_queue_shctx_t;

typedef struct {
    ngx_slab_pool_t              *shpool;
    ngx_http_cache_tag_queue_shctx_t *sh;
    ngx_http_cache_tag_queue_stats_t stats;
} ngx_http_cache_tag_queue_ctx_t;

typedef struct {
    ngx_uint_t                    initialized;
    ngx_uint_t                    active;
    ngx_uint_t                    owner;
    ngx_connection_t             *inotify_conn;
    ngx_event_t                   timer;
    ngx_cycle_t                  *cycle;
    ngx_rbtree_t                  zone_index;
    ngx_rbtree_node_t             zone_sentinel;
    ngx_rbtree_t                  watch_index;
    ngx_rbtree_node_t             watch_sentinel;
    /* Pending store operations accumulated by the inotify read handler.
     * The timer handler drains these to the backing store each tick, then
     * resets the pool to reclaim memory. */
    ngx_pool_t                   *pending_pool;
    ngx_array_t                  *pending_ops;
} ngx_http_cache_tag_watch_runtime_t;

typedef struct {
    ngx_cycle_t                  *cycle;
    ngx_http_cache_tag_store_t   *writer;
    ngx_http_cache_tag_store_t   *reader;
    ngx_flag_t                    owner;
} ngx_http_cache_tag_store_runtime_t;

#define NGX_HTTP_CACHE_TAG_OP_DELETE   1
#define NGX_HTTP_CACHE_TAG_OP_REPLACE  2

#endif

extern ngx_module_t ngx_http_cache_purge_module;

char *ngx_http_cache_tag_index_conf(ngx_conf_t *cf, ngx_command_t *cmd,
                                    void *conf);
char *ngx_http_cache_tag_headers_conf(ngx_conf_t *cf, ngx_command_t *cmd,
                                      void *conf);
ngx_flag_t ngx_http_cache_tag_location_enabled(
    ngx_http_cache_purge_loc_conf_t *cplcf);
ngx_int_t ngx_http_cache_tag_request_headers(ngx_http_request_t *r,
                                             ngx_array_t **tags);
ngx_int_t ngx_http_cache_tag_extract_tokens(ngx_pool_t *pool, u_char *value,
                                            size_t len, ngx_array_t *tags,
                                            ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_register_cache(ngx_conf_t *cf,
                                            ngx_http_file_cache_t *cache,
                                            ngx_array_t *headers);
ngx_int_t ngx_http_cache_tag_purge(ngx_http_request_t *r,
                                   ngx_http_file_cache_t *cache,
                                   ngx_array_t *tags);
ngx_int_t ngx_http_cache_tag_process_init(ngx_cycle_t *cycle,
        ngx_http_cache_purge_main_conf_t *pmcf);
void ngx_http_cache_tag_process_exit(void);
ngx_int_t ngx_http_cache_purge_by_path(ngx_http_file_cache_t *cache,
                                       ngx_str_t *path, ngx_flag_t soft,
                                       ngx_log_t *log);

#if (NGX_LINUX)
ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_open_writer(
    ngx_http_cache_purge_main_conf_t *pmcf, ngx_log_t *log);
ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_open_reader(
    ngx_http_cache_purge_main_conf_t *pmcf, ngx_log_t *log);
void ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_t *store);
ngx_int_t ngx_http_cache_tag_store_begin_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_commit_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_rollback_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_replace_file_tags(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    time_t mtime, off_t size, ngx_array_t *tags, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_delete_file(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_collect_paths_by_tags(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_array_t *tags, ngx_array_t **paths, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_get_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_set_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_process_file(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    ngx_array_t *headers, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_store_runtime_init(
    ngx_cycle_t *cycle, ngx_http_cache_purge_main_conf_t *pmcf,
    ngx_flag_t owner);
void ngx_http_cache_tag_store_runtime_shutdown(void);
ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_writer(void);
ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_reader(
    ngx_http_cache_purge_main_conf_t *pmcf, ngx_log_t *log);

ngx_http_cache_tag_zone_t *ngx_http_cache_tag_lookup_zone(
    ngx_http_file_cache_t *cache);
ngx_int_t ngx_http_cache_tag_bootstrap_zone(
    ngx_http_cache_tag_store_t *store, ngx_http_cache_tag_zone_t *zone,
    ngx_cycle_t *cycle);
ngx_int_t ngx_http_cache_tag_init_runtime(ngx_cycle_t *cycle,
        ngx_http_cache_purge_main_conf_t *pmcf);
ngx_int_t ngx_http_cache_tag_flush_pending(ngx_cycle_t *cycle);
void ngx_http_cache_tag_shutdown_runtime(void);
ngx_flag_t ngx_http_cache_tag_is_owner(void);
ngx_flag_t ngx_http_cache_tag_store_configured(
    ngx_http_cache_purge_main_conf_t *pmcf);
ngx_int_t ngx_http_cache_tag_queue_init_conf(
    ngx_conf_t *cf, ngx_http_cache_purge_main_conf_t *pmcf);
ngx_int_t ngx_http_cache_tag_queue_enqueue_delete(
    ngx_http_cache_purge_main_conf_t *pmcf, ngx_str_t *zone_name,
    ngx_str_t *path, ngx_log_t *log);
#endif

ngx_int_t ngx_http_cache_purge_request_mode(ngx_http_request_t *r,
                                            ngx_flag_t default_soft);

#endif
