#ifndef _NGX_CACHE_PURGE_TAG_STORE_INTERNAL_H_INCLUDED_
#define _NGX_CACHE_PURGE_TAG_STORE_INTERNAL_H_INCLUDED_

#include "ngx_cache_purge_tag.h"

#if (NGX_LINUX)

#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct {
    sqlite3_stmt                 *delete_file;
    sqlite3_stmt                 *insert_entry;
    sqlite3_stmt                 *collect_paths;
    sqlite3_stmt                 *get_zone_state;
    sqlite3_stmt                 *set_zone_state;
} ngx_http_cache_tag_sqlite_stmt_cache_t;

typedef struct ngx_http_cache_tag_store_ops_s
    ngx_http_cache_tag_store_ops_t;

struct ngx_http_cache_tag_store_s {
    const ngx_http_cache_tag_store_ops_t *ops;
    ngx_http_cache_tag_backend_e          backend;
    ngx_flag_t                            readonly;
    union {
        struct {
            sqlite3                      *db;
            ngx_flag_t                    schema_ready;
            ngx_http_cache_tag_sqlite_stmt_cache_t stmt;
        } sqlite;
        struct {
            ngx_socket_t                  fd;
            ngx_http_cache_purge_main_conf_t *pmcf;
            u_char                        recv_buf[4096];
            size_t                        recv_pos;
            size_t                        recv_len;
        } redis;
    } u;
};

struct ngx_http_cache_tag_store_ops_s {
    void (*close)(ngx_http_cache_tag_store_t *store);
    ngx_int_t (*begin_batch)(ngx_http_cache_tag_store_t *store, ngx_log_t *log);
    ngx_int_t (*commit_batch)(ngx_http_cache_tag_store_t *store, ngx_log_t *log);
    ngx_int_t (*rollback_batch)(ngx_http_cache_tag_store_t *store, ngx_log_t *log);
    ngx_int_t (*replace_file_tags)(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, time_t mtime, off_t size,
        ngx_array_t *tags, ngx_log_t *log);
    ngx_int_t (*delete_file)(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, ngx_log_t *log);
    ngx_int_t (*collect_paths_by_tags)(ngx_http_cache_tag_store_t *store,
        ngx_pool_t *pool, ngx_str_t *zone_name, ngx_array_t *tags,
        ngx_array_t **paths, ngx_log_t *log);
    ngx_int_t (*get_zone_state)(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log);
    ngx_int_t (*set_zone_state)(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log);
};

ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_sqlite_open(
    ngx_http_cache_purge_main_conf_t *pmcf, ngx_flag_t readonly, ngx_log_t *log);
ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_redis_open(
    ngx_http_cache_purge_main_conf_t *pmcf, ngx_flag_t readonly, ngx_log_t *log);

#endif

#endif
