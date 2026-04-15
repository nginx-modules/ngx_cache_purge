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
};

#if (NGX_LINUX)
typedef struct {
    ngx_str_t                     zone_name;
    ngx_http_file_cache_t        *cache;
    ngx_str_t                     path;
    int                           wd;
} ngx_http_cache_tag_watch_t;

typedef struct {
    ngx_uint_t                    initialized;
    ngx_uint_t                    active;
    ngx_uint_t                    owner;
    int                           inotify_fd;
    sqlite3                      *db;
    ngx_event_t                   timer;
    ngx_cycle_t                  *cycle;
    ngx_array_t                  *watches;
} ngx_http_cache_tag_runtime_t;
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
                                            ngx_http_file_cache_t *cache);
ngx_int_t ngx_http_cache_tag_purge(ngx_http_request_t *r,
                                   ngx_http_file_cache_t *cache);
ngx_int_t ngx_http_cache_tag_process_init(ngx_cycle_t *cycle,
        ngx_http_cache_purge_main_conf_t *pmcf);
void ngx_http_cache_tag_process_exit(void);
ngx_int_t ngx_http_cache_purge_by_path(ngx_http_file_cache_t *cache,
                                       ngx_str_t *path, ngx_flag_t soft,
                                       ngx_log_t *log);

#if (NGX_LINUX)
sqlite3 *ngx_http_cache_tag_db_open(ngx_str_t *path, int flags, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_sqlite_init(sqlite3 *db, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_db_delete_file(sqlite3 *db, ngx_str_t *zone_name,
                                            ngx_str_t *path, ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_db_replace(sqlite3 *db, ngx_str_t *zone_name,
                                        ngx_str_t *path, time_t mtime,
                                        off_t size, ngx_array_t *tags,
                                        ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_db_collect_paths(sqlite3 *db, ngx_pool_t *pool,
                                              ngx_str_t *zone_name,
                                              ngx_array_t *tags,
                                              ngx_array_t **paths,
                                              ngx_log_t *log);
ngx_int_t ngx_http_cache_tag_process_file(sqlite3 *db, ngx_str_t *zone_name,
                                          ngx_str_t *path, ngx_log_t *log);
ngx_http_cache_tag_zone_t *ngx_http_cache_tag_lookup_zone(
    ngx_http_file_cache_t *cache);
ngx_int_t ngx_http_cache_tag_bootstrap_zone(sqlite3 *db,
        ngx_http_cache_tag_zone_t *zone, ngx_cycle_t *cycle);
ngx_int_t ngx_http_cache_tag_init_runtime(ngx_cycle_t *cycle,
        ngx_http_cache_purge_main_conf_t *pmcf);
void ngx_http_cache_tag_shutdown_runtime(void);
#endif

#endif
