#ifndef _NGX_CACHE_PURGE_TAG_H_INCLUDED_
#define _NGX_CACHE_PURGE_TAG_H_INCLUDED_

#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_LINUX)
#include <sqlite3.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#endif

typedef struct ngx_http_cache_tag_zone_s
    ngx_http_cache_tag_zone_t;

typedef struct {
    ngx_flag_t                    enable;
    ngx_str_t                     method;
    ngx_flag_t                    soft;
    ngx_flag_t                    purge_all;
    ngx_array_t                  *access;   /* array of ngx_in_cidr_t */
    ngx_array_t                  *access6;  /* array of ngx_in6_cidr_t */
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
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    ngx_str_t                     sqlite_path;
    ngx_array_t                  *zones;
} ngx_http_cache_purge_main_conf_t;

struct ngx_http_cache_tag_zone_s {
    ngx_str_t                     zone_name;
    ngx_http_file_cache_t        *cache;
    ngx_array_t                  *headers;
};

#if (NGX_LINUX)

typedef struct {
    sqlite3_stmt                 *delete_file;
    sqlite3_stmt                 *insert_entry;
    sqlite3_stmt                 *collect_paths;
    sqlite3_stmt                 *get_zone_state;
    sqlite3_stmt                 *set_zone_state;
} ngx_http_cache_tag_stmt_cache_t;

typedef struct ngx_http_cache_tag_store_s {
    sqlite3                      *db;
    ngx_flag_t                    readonly;
    ngx_flag_t                    schema_ready;
    ngx_http_cache_tag_stmt_cache_t stmt;
} ngx_http_cache_tag_store_t;

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
    ngx_uint_t                    initialized;
    ngx_uint_t                    active;
    ngx_uint_t                    owner;
    int                           inotify_fd;
    ngx_event_t                   timer;
    ngx_cycle_t                  *cycle;
    ngx_rbtree_t                  zone_index;
    ngx_rbtree_node_t             zone_sentinel;
    ngx_rbtree_t                  watch_index;
    ngx_rbtree_node_t             watch_sentinel;
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
char *ngx_http_cache_tag_watch_conf(ngx_conf_t *cf, ngx_command_t *cmd,
                                    void *conf);
ngx_flag_t ngx_http_cache_tag_location_enabled(
    ngx_http_cache_purge_loc_conf_t *cplcf);
ngx_int_t ngx_http_cache_tag_request_headers(ngx_http_request_t *r,
                                             ngx_array_t **tags);
ngx_int_t ngx_http_cache_tag_extract_tokens(ngx_pool_t *pool, u_char *value,
                                            size_t len, ngx_array_t *tags);
ngx_int_t ngx_http_cache_tag_register_cache(ngx_conf_t *cf,
                                            ngx_http_file_cache_t *cache,
                                            ngx_array_t *headers);
ngx_int_t ngx_http_cache_tag_purge(ngx_http_request_t *r,
                                   ngx_http_file_cache_t *cache);
ngx_int_t ngx_http_cache_tag_process_init(ngx_cycle_t *cycle,
        ngx_http_cache_purge_main_conf_t *pmcf);
void ngx_http_cache_tag_process_exit(void);
ngx_int_t ngx_http_cache_purge_by_path(ngx_http_file_cache_t *cache,
                                       ngx_str_t *path, ngx_flag_t soft,
                                       ngx_log_t *log);

#if (NGX_LINUX)
ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_open_writer(
    ngx_str_t *path, ngx_log_t *log);
ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_open_reader(
    ngx_str_t *path, ngx_log_t *log);
void ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_t *store);
ngx_int_t ngx_http_cache_tag_store_ensure_schema(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
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
#endif

#endif
