#ifndef _NGX_CACHE_PILOT_INDEX_STORE_INTERNAL_H_INCLUDED_
#define _NGX_CACHE_PILOT_INDEX_STORE_INTERNAL_H_INCLUDED_

#include "ngx_cache_pilot_index.h"

#if (NGX_LINUX)

#if (NGX_CACHE_PILOT_SQLITE)
#include <sqlite3.h>

typedef struct {
    sqlite3_stmt                 *delete_file;
    sqlite3_stmt                 *insert_entry;
    sqlite3_stmt                 *collect_paths;
    sqlite3_stmt                 *get_zone_state;
    sqlite3_stmt                 *set_zone_state;
    sqlite3_stmt                 *insert_file_meta;
    sqlite3_stmt                 *delete_file_meta;
    sqlite3_stmt                 *collect_paths_by_key;
    sqlite3_stmt                 *collect_paths_by_key_prefix;
} ngx_http_cache_index_sqlite_stmt_cache_t;
#endif

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct ngx_http_cache_index_store_ops_s
    ngx_http_cache_index_store_ops_t;

struct ngx_http_cache_index_store_s {
    const ngx_http_cache_index_store_ops_t *ops;
    ngx_http_cache_index_backend_e          backend;
    ngx_flag_t                            readonly;
    union {
#if (NGX_CACHE_PILOT_SQLITE)
        struct {
            sqlite3                      *db;
            ngx_flag_t                    schema_ready;
            ngx_http_cache_index_sqlite_stmt_cache_t stmt;
        } sqlite;
#endif
        struct {
            ngx_connection_t             *conn;   /* owns fd lifecycle */
            ngx_socket_t                  fd;     /* cached conn->fd for send/recv */
            ngx_http_cache_pilot_main_conf_t *pmcf;
            u_char                        recv_buf[4096];
            size_t                        recv_pos;
            size_t                        recv_len;
        } redis;
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

#if (NGX_CACHE_PILOT_SQLITE)
ngx_http_cache_index_store_t *ngx_http_cache_index_store_sqlite_open(
    ngx_http_cache_pilot_main_conf_t *pmcf, ngx_flag_t readonly, ngx_log_t *log);
#endif
ngx_http_cache_index_store_t *ngx_http_cache_index_store_redis_open(
    ngx_http_cache_pilot_main_conf_t *pmcf, ngx_flag_t readonly, ngx_log_t *log);

#endif

#endif
