/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * All rights reserved.
 *
 * This project was fully funded by yo.se.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

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


#ifndef nginx_version
    #error This module cannot be build against an unknown nginx version.
#endif

#define NGX_REPONSE_TYPE_HTML 1
#define NGX_REPONSE_TYPE_XML  2
#define NGX_REPONSE_TYPE_JSON 3
#define NGX_REPONSE_TYPE_TEXT 4

static const char ngx_http_cache_purge_content_type_json[] = "application/json";
static const char ngx_http_cache_purge_content_type_html[] = "text/html";
static const char ngx_http_cache_purge_content_type_xml[]  = "text/xml";
static const char ngx_http_cache_purge_content_type_text[] = "text/plain";

static size_t ngx_http_cache_purge_content_type_json_size = sizeof(ngx_http_cache_purge_content_type_json);
static size_t ngx_http_cache_purge_content_type_html_size = sizeof(ngx_http_cache_purge_content_type_html);
static size_t ngx_http_cache_purge_content_type_xml_size = sizeof(ngx_http_cache_purge_content_type_xml);
static size_t ngx_http_cache_purge_content_type_text_size = sizeof(ngx_http_cache_purge_content_type_text);

static const char ngx_http_cache_purge_body_templ_json[] = "{\"Key\": \"%s\"}";
static const char ngx_http_cache_purge_body_templ_html[] =
    "<html><head><title>Successful purge</title></head><body bgcolor=\"white\"><center><h1>Successful purge</h1><p>Key : %s</p></center></body></html>";
static const char ngx_http_cache_purge_body_templ_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><status><Key><![CDATA[%s]]></Key></status>";
static const char ngx_http_cache_purge_body_templ_text[] = "Key:%s\n";

static size_t ngx_http_cache_purge_body_templ_json_size = sizeof(ngx_http_cache_purge_body_templ_json);
static size_t ngx_http_cache_purge_body_templ_html_size = sizeof(ngx_http_cache_purge_body_templ_html);
static size_t ngx_http_cache_purge_body_templ_xml_size = sizeof(ngx_http_cache_purge_body_templ_xml);
static size_t ngx_http_cache_purge_body_templ_text_size = sizeof(ngx_http_cache_purge_body_templ_text);

#if (NGX_HTTP_CACHE)

typedef struct ngx_http_cache_purge_partial_ctx_s
    ngx_http_cache_purge_partial_ctx_t;

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
# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_conf_t   fastcgi;
# endif /* NGX_HTTP_FASTCGI */
# if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_conf_t   proxy;
# endif /* NGX_HTTP_PROXY */
# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_conf_t   scgi;
# endif /* NGX_HTTP_SCGI */
# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_conf_t   uwsgi;
# endif /* NGX_HTTP_UWSGI */

    ngx_http_cache_purge_conf_t  *conf;
    ngx_http_handler_pt           handler;
    ngx_http_handler_pt           original_handler;

    ngx_uint_t                    resptype; /* response content-type */
    ngx_flag_t                    cache_tag_watch;
    ngx_array_t                  *cache_tag_headers; /* array of ngx_str_t */
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    ngx_str_t                     sqlite_path;
    ngx_array_t                  *zones; /* array of ngx_http_cache_tag_zone_t */
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
    ngx_array_t                  *watches; /* array of ngx_http_cache_tag_watch_t */
} ngx_http_cache_tag_runtime_t;

static ngx_http_cache_tag_runtime_t ngx_http_cache_tag_runtime;
#endif

# if (NGX_HTTP_FASTCGI)
char       *ngx_http_fastcgi_cache_purge_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
ngx_int_t   ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r);
# endif /* NGX_HTTP_FASTCGI */

# if (NGX_HTTP_PROXY)
char       *ngx_http_proxy_cache_purge_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
ngx_int_t   ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r);
# endif /* NGX_HTTP_PROXY */

# if (NGX_HTTP_SCGI)
char       *ngx_http_scgi_cache_purge_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
ngx_int_t   ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r);
# endif /* NGX_HTTP_SCGI */

# if (NGX_HTTP_UWSGI)
char       *ngx_http_uwsgi_cache_purge_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
ngx_int_t   ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r);
# endif /* NGX_HTTP_UWSGI */

char        *ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
char        *ngx_http_cache_tag_index_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
char        *ngx_http_cache_tag_headers_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
char        *ngx_http_cache_tag_watch_conf(ngx_conf_t *cf,
        ngx_command_t *cmd, void *conf);
static ngx_int_t
ngx_http_purge_file_cache_noop(ngx_tree_ctx_t *ctx, ngx_str_t *path);
static ngx_int_t
ngx_http_purge_file_cache_delete_file(ngx_tree_ctx_t *ctx, ngx_str_t *path);
static ngx_int_t
ngx_http_purge_file_cache_soft_file(ngx_tree_ctx_t *ctx, ngx_str_t *path);
static ngx_int_t
ngx_http_purge_file_cache_soft_partial_file(ngx_tree_ctx_t *ctx, ngx_str_t *path);
static ngx_int_t
ngx_http_cache_purge_soft_path(ngx_http_file_cache_t *cache, ngx_str_t *path,
                               ngx_log_t *log);
static ngx_int_t
ngx_http_cache_purge_soft_header(ngx_str_t *path, ngx_log_t *log);
static ngx_http_file_cache_node_t *
ngx_http_cache_purge_lookup(ngx_http_file_cache_t *cache, u_char *key);
static ngx_int_t
ngx_http_cache_purge_filename_key(ngx_str_t *path, u_char *key);
static ngx_int_t
ngx_http_cache_purge_partial_match(ngx_http_cache_purge_partial_ctx_t *data,
                                   ngx_str_t *path, ngx_log_t *log);

ngx_int_t   ngx_http_cache_purge_access_handler(ngx_http_request_t *r);
ngx_int_t   ngx_http_cache_purge_access(ngx_array_t *a, ngx_array_t *a6,
                                        struct sockaddr *s);
ngx_int_t   ngx_http_cache_purge_by_path(ngx_http_file_cache_t *cache,
                                         ngx_str_t *path, ngx_flag_t soft,
                                         ngx_log_t *log);
ngx_int_t   ngx_http_cache_tag_purge(ngx_http_request_t *r,
                                     ngx_http_file_cache_t *cache);
ngx_int_t   ngx_http_cache_tag_request_headers(ngx_http_request_t *r,
        ngx_array_t **tags);
ngx_flag_t  ngx_http_cache_tag_location_enabled(
        ngx_http_cache_purge_loc_conf_t *cplcf);
ngx_int_t   ngx_http_cache_tag_register_cache(ngx_conf_t *cf,
        ngx_http_file_cache_t *cache);

ngx_int_t   ngx_http_cache_purge_send_response(ngx_http_request_t *r);
# if (nginx_version >= 1007009)
ngx_int_t   ngx_http_cache_purge_cache_get(ngx_http_request_t *r,
        ngx_http_upstream_t *u, ngx_http_file_cache_t **cache);
# endif /* nginx_version >= 1007009 */
ngx_int_t   ngx_http_cache_purge_init(ngx_http_request_t *r,
                                      ngx_http_file_cache_t *cache, ngx_http_complex_value_t *cache_key);
void        ngx_http_cache_purge_handler(ngx_http_request_t *r);

ngx_int_t   ngx_http_file_cache_purge(ngx_http_request_t *r);
ngx_int_t   ngx_http_file_cache_purge_soft(ngx_http_request_t *r);


ngx_int_t   ngx_http_cache_purge_all(ngx_http_request_t *r, ngx_http_file_cache_t *cache);
ngx_int_t   ngx_http_cache_purge_partial(ngx_http_request_t *r, ngx_http_file_cache_t *cache);
ngx_int_t   ngx_http_cache_purge_is_partial(ngx_http_request_t *r);

char       *ngx_http_cache_purge_conf(ngx_conf_t *cf,
                                      ngx_http_cache_purge_conf_t *cpcf);

void       *ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf);
char       *ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf);
void       *ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf);
char       *ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf,
        void *parent, void *child);
ngx_int_t   ngx_http_cache_purge_init_process(ngx_cycle_t *cycle);
void        ngx_http_cache_purge_exit_process(ngx_cycle_t *cycle);

#if (NGX_LINUX)
static void ngx_http_cache_tag_timer_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_cache_tag_init_runtime(ngx_cycle_t *cycle,
        ngx_http_cache_purge_main_conf_t *pmcf);
static void ngx_http_cache_tag_shutdown_runtime(void);
static ngx_int_t ngx_http_cache_tag_sqlite_init(sqlite3 *db, ngx_log_t *log);
static sqlite3 *ngx_http_cache_tag_db_open(ngx_str_t *path, int flags,
        ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_db_delete_file(sqlite3 *db,
        ngx_str_t *zone_name, ngx_str_t *path, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_db_replace(sqlite3 *db,
        ngx_str_t *zone_name, ngx_str_t *path, time_t mtime, off_t size,
        ngx_array_t *tags, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_db_collect_paths(sqlite3 *db,
        ngx_pool_t *pool, ngx_str_t *zone_name, ngx_array_t *tags,
        ngx_array_t **paths, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_add_watch_recursive(sqlite3 *db,
        ngx_http_cache_tag_zone_t *zone, ngx_str_t *path, ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_add_watch(sqlite3 *db,
        ngx_http_cache_tag_zone_t *zone, ngx_str_t *path, ngx_cycle_t *cycle);
static ngx_http_cache_tag_watch_t *ngx_http_cache_tag_find_watch(int wd);
static ngx_int_t ngx_http_cache_tag_remove_watch(int wd, ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_process_events(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_cache_tag_process_file(sqlite3 *db,
        ngx_str_t *zone_name, ngx_str_t *path, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_parse_file(ngx_pool_t *pool,
        ngx_str_t *path, ngx_array_t *headers, ngx_array_t **tags,
        time_t *mtime, off_t *size, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_extract_tokens(ngx_pool_t *pool,
        u_char *value, size_t len, ngx_array_t *tags);
static ngx_int_t ngx_http_cache_tag_push_unique(ngx_pool_t *pool,
        ngx_array_t *tags, u_char *data, size_t len);
static ngx_int_t ngx_http_cache_tag_join_path(ngx_pool_t *pool,
        ngx_str_t *base, const char *name, ngx_str_t *out);
static ngx_int_t ngx_http_cache_tag_path_known(ngx_str_t *path);
static ngx_http_cache_tag_zone_t *ngx_http_cache_tag_lookup_zone(
        ngx_http_file_cache_t *cache);
#endif

static ngx_command_t  ngx_http_cache_purge_module_commands[] = {
    {
        ngx_string("cache_tag_index"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
        ngx_http_cache_tag_index_conf,
        NGX_HTTP_MAIN_CONF_OFFSET,
        0,
        NULL
    },
    {
        ngx_string("cache_tag_headers"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
        ngx_http_cache_tag_headers_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
    {
        ngx_string("cache_tag_watch"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
        ngx_http_cache_tag_watch_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },

# if (NGX_HTTP_FASTCGI)
    {
        ngx_string("fastcgi_cache_purge"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
        ngx_http_fastcgi_cache_purge_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
# endif /* NGX_HTTP_FASTCGI */

# if (NGX_HTTP_PROXY)
    {
        ngx_string("proxy_cache_purge"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
        ngx_http_proxy_cache_purge_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
# endif /* NGX_HTTP_PROXY */

# if (NGX_HTTP_SCGI)
    {
        ngx_string("scgi_cache_purge"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
        ngx_http_scgi_cache_purge_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
# endif /* NGX_HTTP_SCGI */

# if (NGX_HTTP_UWSGI)
    {
        ngx_string("uwsgi_cache_purge"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
        ngx_http_uwsgi_cache_purge_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
# endif /* NGX_HTTP_UWSGI */


    {
        ngx_string("cache_purge_response_type"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_http_cache_purge_response_type_conf,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },

    ngx_null_command
};

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    ngx_http_cache_purge_create_main_conf, /* create main configuration */
    ngx_http_cache_purge_init_main_conf,   /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_cache_purge_create_loc_conf,  /* create location configuration */
    ngx_http_cache_purge_merge_loc_conf    /* merge location configuration */
};

ngx_module_t  ngx_http_cache_purge_module = {
    NGX_MODULE_V1,
    &ngx_http_cache_purge_module_ctx,      /* module context */
    ngx_http_cache_purge_module_commands,  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_cache_purge_init_process,     /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_http_cache_purge_exit_process,     /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

# if (NGX_HTTP_FASTCGI)
extern ngx_module_t  ngx_http_fastcgi_module;

#  if (nginx_version >= 1007009)

typedef struct {
    ngx_array_t                    caches;  /* ngx_http_file_cache_t * */
} ngx_http_fastcgi_main_conf_t;

#  endif /* nginx_version >= 1007009 */

#  if (nginx_version >= 1007008)

typedef struct {
    ngx_array_t                   *flushes;
    ngx_array_t                   *lengths;
    ngx_array_t                   *values;
    ngx_uint_t                     number;
    ngx_hash_t                     hash;
} ngx_http_fastcgi_params_t;

#  endif /* nginx_version >= 1007008 */

typedef struct {
    ngx_http_upstream_conf_t       upstream;

    ngx_str_t                      index;

#  if (nginx_version >= 1007008)
    ngx_http_fastcgi_params_t      params;
    ngx_http_fastcgi_params_t      params_cache;
#  else
    ngx_array_t                   *flushes;
    ngx_array_t                   *params_len;
    ngx_array_t                   *params;
#  endif /* nginx_version >= 1007008 */

    ngx_array_t                   *params_source;
    ngx_array_t                   *catch_stderr;

    ngx_array_t                   *fastcgi_lengths;
    ngx_array_t                   *fastcgi_values;

#  if (nginx_version >= 8040) && (nginx_version < 1007008)
    ngx_hash_t                     headers_hash;
    ngx_uint_t                     header_params;
#  endif /* nginx_version >= 8040 && nginx_version < 1007008 */

#  if (nginx_version >= 1001004)
    ngx_flag_t                     keep_conn;
#  endif /* nginx_version >= 1001004 */

    ngx_http_complex_value_t       cache_key;

#  if (NGX_PCRE)
    ngx_regex_t                   *split_regex;
    ngx_str_t                      split_name;
#  endif /* NGX_PCRE */
} ngx_http_fastcgi_loc_conf_t;

char *
ngx_http_fastcgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
                                  void *conf) {
    ngx_http_compile_complex_value_t   ccv;
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_http_core_loc_conf_t          *clcf;
    ngx_http_fastcgi_loc_conf_t       *flcf;
    ngx_str_t                         *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t           cv;
#  endif /* nginx_version >= 1007009 */

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->fastcgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (cf->args->nelts != 3
            && !(cf->args->nelts == 4
                 && ngx_strcmp(value[3].data, "soft") == 0)) {
        return ngx_http_cache_purge_conf(cf, &cplcf->fastcgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    flcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_fastcgi_module);

#  if (nginx_version >= 1007009)
    if (flcf->upstream.cache > 0)
#  else
    if (flcf->upstream.cache != NGX_CONF_UNSET_PTR
            && flcf->upstream.cache != NULL)
#  endif /* nginx_version >= 1007009 */
    {
        return "is incompatible with \"fastcgi_cache\"";
    }

    if (flcf->upstream.upstream || flcf->fastcgi_lengths) {
        return "is incompatible with \"fastcgi_pass\"";
    }

    if (flcf->upstream.store > 0
#  if (nginx_version < 1007009)
            || flcf->upstream.store_lengths
#  endif /* nginx_version >= 1007009 */
       ) {
        return "is incompatible with \"fastcgi_store\"";
    }

    if (cf->args->nelts == 4) {
        cplcf->fastcgi.soft = 1;
    }

    /* set fastcgi_cache part */
#  if (nginx_version >= 1007009)

    flcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {

        flcf->upstream.cache_value = ngx_palloc(cf->pool,
                                                sizeof(ngx_http_complex_value_t));
        if (flcf->upstream.cache_value == NULL) {
            return NGX_CONF_ERROR;
        }

        *flcf->upstream.cache_value = cv;

    } else {

        flcf->upstream.cache_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                    &ngx_http_fastcgi_module);
        if (flcf->upstream.cache_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }

#  else

    flcf->upstream.cache = ngx_shared_memory_add(cf, &value[1], 0,
                           &ngx_http_fastcgi_module);
    if (flcf->upstream.cache == NULL) {
        return NGX_CONF_ERROR;
    }

#  endif /* nginx_version >= 1007009 */

    /* set fastcgi_cache_key part */
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &flcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    /* set handler */
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    cplcf->fastcgi.enable = 0;
    cplcf->conf = &cplcf->fastcgi;
    clcf->handler = ngx_http_fastcgi_cache_purge_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_fastcgi_loc_conf_t         *flcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
#  if (nginx_version >= 1007009)
    ngx_http_fastcgi_main_conf_t        *fmcf;
    ngx_int_t                           rc;
#  endif /* nginx_version >= 1007009 */

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    flcf = ngx_http_get_module_loc_conf(r, ngx_http_fastcgi_module);

    r->upstream->conf = &flcf->upstream;

#  if (nginx_version >= 1007009)

    fmcf = ngx_http_get_module_main_conf(r, ngx_http_fastcgi_module);

    r->upstream->caches = &fmcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return rc;
    }

#  else

    cache = flcf->upstream.cache->data;

#  endif /* nginx_version >= 1007009 */

    if (ngx_http_cache_purge_init(r, cache, &flcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = NGX_OK;

    /* Purge-all option */
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (ngx_http_cache_tag_location_enabled(cplcf)) {
        rc = ngx_http_cache_tag_purge(r, cache);
    } else if (cplcf->conf->purge_all) {
        rc = ngx_http_cache_purge_all(r, cache);
    } else {
        if (ngx_http_cache_purge_is_partial(r)) {
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                          "http file cache purge with partial enabled");

            rc = ngx_http_cache_purge_partial(r, cache);
        }
    }

    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

#  if (nginx_version >= 8011)
    r->main->count++;
#  endif

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_FASTCGI */

# if (NGX_HTTP_PROXY)
extern ngx_module_t  ngx_http_proxy_module;

typedef struct {
    ngx_str_t                      key_start;
    ngx_str_t                      schema;
    ngx_str_t                      host_header;
    ngx_str_t                      port;
    ngx_str_t                      uri;
} ngx_http_proxy_vars_t;

#  if (nginx_version >= 1007009)

typedef struct {
    ngx_array_t                    caches;  /* ngx_http_file_cache_t * */
} ngx_http_proxy_main_conf_t;

#  endif /* nginx_version >= 1007009 */

#  if (nginx_version >= 1007008)

typedef struct {
    ngx_array_t                   *flushes;
    ngx_array_t                   *lengths;
    ngx_array_t                   *values;
    ngx_hash_t                     hash;
} ngx_http_proxy_headers_t;

#  endif /* nginx_version >= 1007008 */

typedef struct {
    ngx_http_upstream_conf_t       upstream;

#  if (nginx_version >= 1007008)
    ngx_array_t                   *body_flushes;
    ngx_array_t                   *body_lengths;
    ngx_array_t                   *body_values;
    ngx_str_t                      body_source;

    ngx_http_proxy_headers_t       headers;
    ngx_http_proxy_headers_t       headers_cache;
#  else
    ngx_array_t                   *flushes;
    ngx_array_t                   *body_set_len;
    ngx_array_t                   *body_set;
    ngx_array_t                   *headers_set_len;
    ngx_array_t                   *headers_set;
    ngx_hash_t                     headers_set_hash;
#  endif /* nginx_version >= 1007008 */

    ngx_array_t                   *headers_source;
#  if (nginx_version >= 1029004)
    ngx_uint_t                     host_set;
#  endif /* nginx_version >= 1029004 */
#  if (nginx_version < 8040)
    ngx_array_t                   *headers_names;
#  endif /* nginx_version < 8040 */

    ngx_array_t                   *proxy_lengths;
    ngx_array_t                   *proxy_values;

    ngx_array_t                   *redirects;
#  if (nginx_version >= 1001015)
    ngx_array_t                   *cookie_domains;
    ngx_array_t                   *cookie_paths;
#  endif /* nginx_version >= 1001015 */
#  if (nginx_version >= 1019003)
    ngx_array_t                   *cookie_flags;
#  endif /* nginx_version >= 1019003 */
#  if (nginx_version < 1007008)
    ngx_str_t                      body_source;
#  endif /* nginx_version < 1007008 */

#  if (nginx_version >= 1011006)
    ngx_http_complex_value_t      *method;
#  else
    ngx_str_t                      method;
#  endif /* nginx_version >= 1011006 */
    ngx_str_t                      location;
    ngx_str_t                      url;

    ngx_http_complex_value_t       cache_key;

    ngx_http_proxy_vars_t          vars;

    ngx_flag_t                     redirect;

#  if (nginx_version >= 1001004)
    ngx_uint_t                     http_version;
#  endif /* nginx_version >= 1001004 */

    ngx_uint_t                     headers_hash_max_size;
    ngx_uint_t                     headers_hash_bucket_size;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005006)
    ngx_uint_t                     ssl;
    ngx_uint_t                     ssl_protocols;
    ngx_str_t                      ssl_ciphers;
#    endif /* nginx_version >= 1005006 */
#    if (nginx_version >= 1007000)
    ngx_uint_t                     ssl_verify_depth;
    ngx_str_t                      ssl_trusted_certificate;
    ngx_str_t                      ssl_crl;
#    endif /* nginx_version >= 1007000 */
#    if ((nginx_version >= 1007008) && (nginx_version < 1021000))
    ngx_str_t                      ssl_certificate;
    ngx_str_t                      ssl_certificate_key;
    ngx_array_t                   *ssl_passwords;
#    endif /* nginx_version >= 1007008 && nginx_version < 1021000 */
#    if (nginx_version >= 1019004)
    ngx_array_t                   *ssl_conf_commands;
#    endif /*nginx_version >= 1019004 */
#  endif
} ngx_http_proxy_loc_conf_t;

char *
ngx_http_proxy_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_compile_complex_value_t   ccv;
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_http_core_loc_conf_t          *clcf;
    ngx_http_proxy_loc_conf_t         *plcf;
    ngx_str_t                         *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t           cv;
#  endif /* nginx_version >= 1007009 */

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->proxy.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (cf->args->nelts != 3
            && !(cf->args->nelts == 4
                 && ngx_strcmp(value[3].data, "soft") == 0)) {
        return ngx_http_cache_purge_conf(cf, &cplcf->proxy);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_proxy_module);

#  if (nginx_version >= 1007009)
    if (plcf->upstream.cache > 0)
#  else
    if (plcf->upstream.cache != NGX_CONF_UNSET_PTR
            && plcf->upstream.cache != NULL)
#  endif /* nginx_version >= 1007009 */
    {
        return "is incompatible with \"proxy_cache\"";
    }

    if (plcf->upstream.upstream || plcf->proxy_lengths) {
        return "is incompatible with \"proxy_pass\"";
    }

    if (plcf->upstream.store > 0
#  if (nginx_version < 1007009)
            || plcf->upstream.store_lengths
#  endif /* nginx_version >= 1007009 */
       ) {
        return "is incompatible with \"proxy_store\"";
    }

    if (cf->args->nelts == 4) {
        cplcf->proxy.soft = 1;
    }

    /* set proxy_cache part */
#  if (nginx_version >= 1007009)

    plcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {

        plcf->upstream.cache_value = ngx_palloc(cf->pool,
                                                sizeof(ngx_http_complex_value_t));
        if (plcf->upstream.cache_value == NULL) {
            return NGX_CONF_ERROR;
        }

        *plcf->upstream.cache_value = cv;

    } else {

        plcf->upstream.cache_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                    &ngx_http_proxy_module);
        if (plcf->upstream.cache_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }

#  else

    plcf->upstream.cache = ngx_shared_memory_add(cf, &value[1], 0,
                           &ngx_http_proxy_module);
    if (plcf->upstream.cache == NULL) {
        return NGX_CONF_ERROR;
    }

#  endif /* nginx_version >= 1007009 */

    /* set proxy_cache_key part */
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &plcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    /* set handler */
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    cplcf->proxy.enable = 0;
    cplcf->conf = &cplcf->proxy;
    clcf->handler = ngx_http_proxy_cache_purge_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_proxy_loc_conf_t           *plcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
#  if (nginx_version >= 1007009)
    ngx_http_proxy_main_conf_t          *pmcf;
    ngx_int_t                            rc;
#  endif /* nginx_version >= 1007009 */

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_proxy_module);

    r->upstream->conf = &plcf->upstream;

#  if (nginx_version >= 1007009)

    pmcf = ngx_http_get_module_main_conf(r, ngx_http_proxy_module);

    r->upstream->caches = &pmcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return rc;
    }

#  else

    cache = plcf->upstream.cache->data;

#  endif /* nginx_version >= 1007009 */

    if (ngx_http_cache_purge_init(r, cache, &plcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = NGX_OK;

    /* Purge-all option */
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (ngx_http_cache_tag_location_enabled(cplcf)) {
        rc = ngx_http_cache_tag_purge(r, cache);
    } else if (cplcf->conf->purge_all) {
        rc = ngx_http_cache_purge_all(r, cache);
    } else {
        if (ngx_http_cache_purge_is_partial(r)) {
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                          "http file cache purge with partial enabled");

            rc = ngx_http_cache_purge_partial(r, cache);
        }
    }

    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

#  if (nginx_version >= 8011)
    r->main->count++;
#  endif

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_PROXY */

# if (NGX_HTTP_SCGI)
extern ngx_module_t  ngx_http_scgi_module;

#  if (nginx_version >= 1007009)

typedef struct {
    ngx_array_t                caches;  /* ngx_http_file_cache_t * */
} ngx_http_scgi_main_conf_t;

#  endif /* nginx_version >= 1007009 */

#  if (nginx_version >= 1007008)

typedef struct {
    ngx_array_t               *flushes;
    ngx_array_t               *lengths;
    ngx_array_t               *values;
    ngx_uint_t                 number;
    ngx_hash_t                 hash;
} ngx_http_scgi_params_t;

#  endif /* nginx_version >= 1007008 */

typedef struct {
    ngx_http_upstream_conf_t   upstream;

#  if (nginx_version >= 1007008)
    ngx_http_scgi_params_t     params;
    ngx_http_scgi_params_t     params_cache;
    ngx_array_t               *params_source;
#  else
    ngx_array_t               *flushes;
    ngx_array_t               *params_len;
    ngx_array_t               *params;
    ngx_array_t               *params_source;

    ngx_hash_t                 headers_hash;
    ngx_uint_t                 header_params;
#  endif /* nginx_version >= 1007008 */

    ngx_array_t               *scgi_lengths;
    ngx_array_t               *scgi_values;

    ngx_http_complex_value_t   cache_key;
} ngx_http_scgi_loc_conf_t;

char *
ngx_http_scgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_compile_complex_value_t   ccv;
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_http_core_loc_conf_t          *clcf;
    ngx_http_scgi_loc_conf_t          *slcf;
    ngx_str_t                         *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t           cv;
#  endif /* nginx_version >= 1007009 */

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->scgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (cf->args->nelts != 3
            && !(cf->args->nelts == 4
                 && ngx_strcmp(value[3].data, "soft") == 0)) {
        return ngx_http_cache_purge_conf(cf, &cplcf->scgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    slcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_scgi_module);

#  if (nginx_version >= 1007009)
    if (slcf->upstream.cache > 0)
#  else
    if (slcf->upstream.cache != NGX_CONF_UNSET_PTR
            && slcf->upstream.cache != NULL)
#  endif /* nginx_version >= 1007009 */
    {
        return "is incompatible with \"scgi_cache\"";
    }

    if (slcf->upstream.upstream || slcf->scgi_lengths) {
        return "is incompatible with \"scgi_pass\"";
    }

    if (slcf->upstream.store > 0
#  if (nginx_version < 1007009)
            || slcf->upstream.store_lengths
#  endif /* nginx_version >= 1007009 */
       ) {
        return "is incompatible with \"scgi_store\"";
    }

    if (cf->args->nelts == 4) {
        cplcf->scgi.soft = 1;
    }

    /* set scgi_cache part */
#  if (nginx_version >= 1007009)

    slcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {

        slcf->upstream.cache_value = ngx_palloc(cf->pool,
                                                sizeof(ngx_http_complex_value_t));
        if (slcf->upstream.cache_value == NULL) {
            return NGX_CONF_ERROR;
        }

        *slcf->upstream.cache_value = cv;

    } else {

        slcf->upstream.cache_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                    &ngx_http_scgi_module);
        if (slcf->upstream.cache_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }

#  else

    slcf->upstream.cache = ngx_shared_memory_add(cf, &value[1], 0,
                           &ngx_http_scgi_module);
    if (slcf->upstream.cache == NULL) {
        return NGX_CONF_ERROR;
    }

#  endif /* nginx_version >= 1007009 */

    /* set scgi_cache_key part */
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &slcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    /* set handler */
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    cplcf->scgi.enable = 0;
    cplcf->conf = &cplcf->scgi;
    clcf->handler = ngx_http_scgi_cache_purge_handler;

    return NGX_CONF_OK;
}

ngx_int_t
ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_scgi_loc_conf_t            *slcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
#  if (nginx_version >= 1007009)
    ngx_http_scgi_main_conf_t           *smcf;
    ngx_int_t                           rc;
#  endif /* nginx_version >= 1007009 */

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_scgi_module);

    r->upstream->conf = &slcf->upstream;

#  if (nginx_version >= 1007009)

    smcf = ngx_http_get_module_main_conf(r, ngx_http_scgi_module);

    r->upstream->caches = &smcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return rc;
    }

#  else

    cache = slcf->upstream.cache->data;

#  endif /* nginx_version >= 1007009 */

    if (ngx_http_cache_purge_init(r, cache, &slcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = NGX_OK;

    /* Purge-all option */
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (ngx_http_cache_tag_location_enabled(cplcf)) {
        rc = ngx_http_cache_tag_purge(r, cache);
    } else if (cplcf->conf->purge_all) {
        rc = ngx_http_cache_purge_all(r, cache);
    } else {
        if (ngx_http_cache_purge_is_partial(r)) {
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                          "http file cache purge with partial enabled");

            rc = ngx_http_cache_purge_partial(r, cache);
        }
    }

    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

#  if (nginx_version >= 8011)
    r->main->count++;
#  endif

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_SCGI */

# if (NGX_HTTP_UWSGI)
extern ngx_module_t  ngx_http_uwsgi_module;

#  if (nginx_version >= 1007009)

typedef struct {
    ngx_array_t                caches;  /* ngx_http_file_cache_t * */
} ngx_http_uwsgi_main_conf_t;

#  endif /* nginx_version >= 1007009 */

#  if (nginx_version >= 1007008)

typedef struct {
    ngx_array_t               *flushes;
    ngx_array_t               *lengths;
    ngx_array_t               *values;
    ngx_uint_t                 number;
    ngx_hash_t                 hash;
} ngx_http_uwsgi_params_t;

#  endif /* nginx_version >= 1007008 */

typedef struct {
    ngx_http_upstream_conf_t   upstream;

#  if (nginx_version >= 1007008)
    ngx_http_uwsgi_params_t    params;
    ngx_http_uwsgi_params_t    params_cache;
    ngx_array_t               *params_source;
#  else
    ngx_array_t               *flushes;
    ngx_array_t               *params_len;
    ngx_array_t               *params;
    ngx_array_t               *params_source;

    ngx_hash_t                 headers_hash;
    ngx_uint_t                 header_params;
#  endif /* nginx_version >= 1007008 */

    ngx_array_t               *uwsgi_lengths;
    ngx_array_t               *uwsgi_values;

    ngx_http_complex_value_t   cache_key;

    ngx_str_t                  uwsgi_string;

    ngx_uint_t                 modifier1;
    ngx_uint_t                 modifier2;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005008)
    ngx_uint_t                 ssl;
    ngx_uint_t                 ssl_protocols;
    ngx_str_t                  ssl_ciphers;
#    endif /* nginx_version >= 1005008 */
#    if (nginx_version >= 1007000)
    ngx_uint_t                 ssl_verify_depth;
    ngx_str_t                  ssl_trusted_certificate;
    ngx_str_t                  ssl_crl;
#    endif /* nginx_version >= 1007000 */
#    if ((nginx_version >= 1007008) && (nginx_version < 1021000))
    ngx_str_t                  ssl_certificate;
    ngx_str_t                  ssl_certificate_key;
    ngx_array_t               *ssl_passwords;
#    endif /* nginx_version >= 1007008 && nginx_version < 1021000 */
#    if (nginx_version >= 1019004)
    ngx_array_t               *ssl_conf_commands;
#    endif /*nginx_version >= 1019004 */
#  endif
} ngx_http_uwsgi_loc_conf_t;

char *
ngx_http_uwsgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_compile_complex_value_t   ccv;
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_http_core_loc_conf_t          *clcf;
    ngx_http_uwsgi_loc_conf_t         *ulcf;
    ngx_str_t                         *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t           cv;
#  endif /* nginx_version >= 1007009 */

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->uwsgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (cf->args->nelts != 3
            && !(cf->args->nelts == 4
                 && ngx_strcmp(value[3].data, "soft") == 0)) {
        return ngx_http_cache_purge_conf(cf, &cplcf->uwsgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        return "(separate location syntax) is not allowed here";
    }

    ulcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_uwsgi_module);

#  if (nginx_version >= 1007009)
    if (ulcf->upstream.cache > 0)
#  else
    if (ulcf->upstream.cache != NGX_CONF_UNSET_PTR
            && ulcf->upstream.cache != NULL)
#  endif /* nginx_version >= 1007009 */
    {
        return "is incompatible with \"uwsgi_cache\"";
    }

    if (ulcf->upstream.upstream || ulcf->uwsgi_lengths) {
        return "is incompatible with \"uwsgi_pass\"";
    }

    if (ulcf->upstream.store > 0
#  if (nginx_version < 1007009)
            || ulcf->upstream.store_lengths
#  endif /* nginx_version >= 1007009 */
       ) {
        return "is incompatible with \"uwsgi_store\"";
    }

    if (cf->args->nelts == 4) {
        cplcf->uwsgi.soft = 1;
    }

    /* set uwsgi_cache part */
#  if (nginx_version >= 1007009)

    ulcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {

        ulcf->upstream.cache_value = ngx_palloc(cf->pool,
                                                sizeof(ngx_http_complex_value_t));
        if (ulcf->upstream.cache_value == NULL) {
            return NGX_CONF_ERROR;
        }

        *ulcf->upstream.cache_value = cv;

    } else {

        ulcf->upstream.cache_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                    &ngx_http_uwsgi_module);
        if (ulcf->upstream.cache_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }

#  else

    ulcf->upstream.cache = ngx_shared_memory_add(cf, &value[1], 0,
                           &ngx_http_uwsgi_module);
    if (ulcf->upstream.cache == NULL) {
        return NGX_CONF_ERROR;
    }

#  endif /* nginx_version >= 1007009 */

    /* set uwsgi_cache_key part */
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &ulcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    /* set handler */
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    cplcf->uwsgi.enable = 0;
    cplcf->conf = &cplcf->uwsgi;
    clcf->handler = ngx_http_uwsgi_cache_purge_handler;

    return NGX_CONF_OK;
}


ngx_int_t
ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_uwsgi_loc_conf_t           *ulcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
#  if (nginx_version >= 1007009)
    ngx_http_uwsgi_main_conf_t          *umcf;
    ngx_int_t                           rc;
#  endif /* nginx_version >= 1007009 */

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ulcf = ngx_http_get_module_loc_conf(r, ngx_http_uwsgi_module);

    r->upstream->conf = &ulcf->upstream;

#  if (nginx_version >= 1007009)

    umcf = ngx_http_get_module_main_conf(r, ngx_http_uwsgi_module);

    r->upstream->caches = &umcf->caches;

    rc = ngx_http_cache_purge_cache_get(r, r->upstream, &cache);
    if (rc != NGX_OK) {
        return rc;
    }

#  else

    cache = ulcf->upstream.cache->data;

#  endif /* nginx_version >= 1007009 */

    if (ngx_http_cache_purge_init(r, cache, &ulcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = NGX_OK;

    /* Purge-all option */
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (ngx_http_cache_tag_location_enabled(cplcf)) {
        rc = ngx_http_cache_tag_purge(r, cache);
    } else if (cplcf->conf->purge_all) {
        rc = ngx_http_cache_purge_all(r, cache);
    } else {
        if (ngx_http_cache_purge_is_partial(r)) {
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                          "http file cache purge with partial enabled");

            rc = ngx_http_cache_purge_partial(r, cache);
        }
    }

    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

#  if (nginx_version >= 8011)
    r->main->count++;
#  endif

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_UWSGI */


char *
ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_str_t                         *value;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->resptype != NGX_CONF_UNSET_UINT && cf->cmd_type == NGX_HTTP_LOC_CONF)  {
        return "is duplicate";
    }

    /* sanity check */
    if (cf->args->nelts < 2) {
        return "is invalid paramter, ex) cache_purge_response_type (html|json|xml|text)";
    }

    if (cf->args->nelts > 2) {
        return "is required only 1 option, ex) cache_purge_response_type (html|json|xml|text)";
    }

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "html") != 0 && ngx_strcmp(value[1].data, "json") != 0
            && ngx_strcmp(value[1].data, "xml") != 0 && ngx_strcmp(value[1].data, "text") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\", expected"
                           " \"(html|json|xml|text)\" keyword", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (cf->cmd_type == NGX_HTTP_MODULE) {
        return "(separate server or location syntax) is not allowed here";
    }

    if (ngx_strcmp(value[1].data, "html") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_HTML;
    } else if (ngx_strcmp(value[1].data, "xml") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_XML;
    } else if (ngx_strcmp(value[1].data, "json") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_JSON;
    } else if (ngx_strcmp(value[1].data, "text") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_TEXT;
    }

    return NGX_CONF_OK;
}

char *
ngx_http_cache_tag_index_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_main_conf_t  *pmcf;
    ngx_str_t                         *value;

    pmcf = conf;
    value = cf->args->elts;

    if (pmcf->sqlite_path.data != NULL) {
        return "is duplicate";
    }

    if (ngx_strcmp(value[1].data, "sqlite") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid cache_tag_index backend \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

#if !(NGX_LINUX)
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "cache_tag_index requires Linux inotify support");
    return NGX_CONF_ERROR;
#else
    pmcf->sqlite_path = value[2];
    return NGX_CONF_OK;
#endif
}

char *
ngx_http_cache_tag_headers_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_str_t                        *value, *header;
    ngx_uint_t                        i;

    cplcf = conf;

    if (cplcf->cache_tag_headers != NULL && cf->cmd_type == NGX_HTTP_LOC_CONF) {
        return "is duplicate";
    }

    cplcf->cache_tag_headers = ngx_array_create(cf->pool, cf->args->nelts - 1,
                                                sizeof(ngx_str_t));
    if (cplcf->cache_tag_headers == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        header = ngx_array_push(cplcf->cache_tag_headers);
        if (header == NULL) {
            return NGX_CONF_ERROR;
        }

        *header = value[i];
    }

    return NGX_CONF_OK;
}

char *
ngx_http_cache_tag_watch_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_str_t                        *value;

    cplcf = conf;
    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "on") == 0) {
        cplcf->cache_tag_watch = 1;
        return NGX_CONF_OK;
    }

    if (ngx_strcmp(value[1].data, "off") == 0) {
        cplcf->cache_tag_watch = 0;
        return NGX_CONF_OK;
    }

    return "invalid value";
}

static ngx_int_t
ngx_http_purge_file_cache_noop(ngx_tree_ctx_t *ctx, ngx_str_t *path) {
    return NGX_OK;
}

static ngx_int_t
ngx_http_purge_file_cache_delete_file(ngx_tree_ctx_t *ctx, ngx_str_t *path) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->log, 0,
                   "http file cache delete: \"%s\"", path->data);

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", path->data);
    }

    return NGX_OK;
}


struct ngx_http_cache_purge_partial_ctx_s {
    ngx_http_file_cache_t *cache;
    u_char *key_partial;
    u_char *key_in_file;
    ngx_uint_t key_len;
};

static ngx_int_t
ngx_http_purge_file_cache_delete_partial_file(ngx_tree_ctx_t *ctx, ngx_str_t *path) {
    if (!ngx_http_cache_purge_partial_match(ctx->data, path, ctx->log)) {
        return NGX_OK;
    }

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", path->data);
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_purge_file_cache_soft_file(ngx_tree_ctx_t *ctx, ngx_str_t *path) {
    ngx_http_cache_purge_partial_ctx_t *data;

    data = ctx->data;

    return ngx_http_cache_purge_soft_path(data->cache, path, ctx->log);
}

static ngx_int_t
ngx_http_purge_file_cache_soft_partial_file(ngx_tree_ctx_t *ctx, ngx_str_t *path) {
    ngx_http_cache_purge_partial_ctx_t *data;

    data = ctx->data;

    if (!ngx_http_cache_purge_partial_match(data, path, ctx->log)) {
        return NGX_OK;
    }

    return ngx_http_cache_purge_soft_path(data->cache, path, ctx->log);
}

static ngx_int_t
ngx_http_cache_purge_partial_match(ngx_http_cache_purge_partial_ctx_t *data,
                                   ngx_str_t *path, ngx_log_t *log) {
    ngx_file_t    file;
    ssize_t       n;

    /* if key_partial is empty always match, because it is a '*' */
    if (data->key_len == 0) {
        ngx_log_debug(NGX_LOG_DEBUG_HTTP, log, 0,
                      "empty key_partial, forcing purge");
        return 1;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.offset = file.sys_offset = 0;
    file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN,
                            NGX_FILE_DEFAULT_ACCESS);
    if (file.fd == NGX_INVALID_FILE) {
        return 0;
    }
    file.log = log;

    ngx_memzero(data->key_in_file, sizeof(u_char) * data->key_len);

    n = ngx_read_file(&file, data->key_in_file, sizeof(u_char) * data->key_len,
                      sizeof(ngx_http_file_cache_header_t) + sizeof(u_char) * 6);
    ngx_close_file(file.fd);

    if (n == NGX_ERROR || (size_t) n != sizeof(u_char) * data->key_len) {
        return 0;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
                   "http cache file \"%s\" key read: \"%s\"",
                   path->data, data->key_in_file);

    if (ngx_strncasecmp(data->key_in_file, data->key_partial,
                        data->key_len) == 0) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                       "match found for file \"%s\"", path->data);
        return 1;
    }

    return 0;
}

static ngx_int_t
ngx_http_cache_purge_filename_key(ngx_str_t *path, u_char *key) {
    u_char      *p;
    ngx_uint_t   i;
    ngx_uint_t   hi, lo;

    p = path->data + path->len;

    while (p > path->data && p[-1] != '/') {
        p--;
    }

    if ((size_t)(path->data + path->len - p) != 2 * NGX_HTTP_CACHE_KEY_LEN) {
        return NGX_DECLINED;
    }

    for (i = 0; i < NGX_HTTP_CACHE_KEY_LEN; i++) {
        hi = ngx_hextoi(p + i * 2, 1);
        lo = ngx_hextoi(p + i * 2 + 1, 1);

        if (hi == (ngx_uint_t) NGX_ERROR || lo == (ngx_uint_t) NGX_ERROR) {
            return NGX_DECLINED;
        }

        key[i] = (u_char)((hi << 4) | lo);
    }

    return NGX_OK;
}

static ngx_http_file_cache_node_t *
ngx_http_cache_purge_lookup(ngx_http_file_cache_t *cache, u_char *key) {
    ngx_int_t                    rc;
    ngx_rbtree_key_t             node_key;
    ngx_rbtree_node_t           *node, *sentinel;
    ngx_http_file_cache_node_t  *fcn;

    ngx_memcpy((u_char *) &node_key, key, sizeof(ngx_rbtree_key_t));

    node = cache->sh->rbtree.root;
    sentinel = cache->sh->rbtree.sentinel;

    while (node != sentinel) {
        if (node_key < node->key) {
            node = node->left;
            continue;
        }

        if (node_key > node->key) {
            node = node->right;
            continue;
        }

        fcn = (ngx_http_file_cache_node_t *) node;

        rc = ngx_memcmp(&key[sizeof(ngx_rbtree_key_t)], fcn->key,
                        NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));

        if (rc == 0) {
            return fcn;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}

static ngx_int_t
ngx_http_cache_purge_soft_header(ngx_str_t *path, ngx_log_t *log) {
    ngx_file_t                     file;
    ngx_http_file_cache_header_t   h;
    ssize_t                        n;

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = *path;
    file.log = log;
    file.fd = ngx_open_file(path->data, NGX_FILE_RDWR, NGX_FILE_OPEN, 0);

    if (file.fd == NGX_INVALID_FILE) {
        if (ngx_errno == NGX_ENOENT) {
            return NGX_DECLINED;
        }

        ngx_log_error(NGX_LOG_CRIT, log, ngx_errno,
                      ngx_open_file_n " \"%s\" failed", path->data);
        return NGX_ERROR;
    }

    n = ngx_read_file(&file, (u_char *) &h, sizeof(h), 0);
    if (n == NGX_ERROR || (size_t) n != sizeof(h)) {
        ngx_close_file(file.fd);

        if (n == NGX_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, log, ngx_errno,
                          ngx_read_file_n " \"%s\" failed", path->data);
        } else {
            ngx_log_error(NGX_LOG_CRIT, log, 0,
                          "cache file \"%s\" header read was incomplete",
                          path->data);
        }

        return NGX_ERROR;
    }

    if (h.version != NGX_HTTP_CACHE_VERSION) {
        ngx_close_file(file.fd);

        ngx_log_error(NGX_LOG_CRIT, log, 0,
                      "cache file \"%s\" header version mismatch",
                      path->data);
        return NGX_ERROR;
    }

    h.valid_sec = 0;

    n = ngx_write_file(&file, (u_char *) &h, sizeof(h), 0);
    if (n == NGX_ERROR || (size_t) n != sizeof(h)) {
        ngx_close_file(file.fd);

        if (n == NGX_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, log, ngx_errno,
                          "ngx_write_file() \"%s\" failed", path->data);
        } else {
            ngx_log_error(NGX_LOG_CRIT, log, 0,
                          "cache file \"%s\" header write was incomplete",
                          path->data);
        }

        return NGX_ERROR;
    }

    ngx_close_file(file.fd);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_purge_soft_path(ngx_http_file_cache_t *cache, ngx_str_t *path,
                               ngx_log_t *log) {
    ngx_int_t                      rc;
    ngx_http_file_cache_node_t  *node;
    u_char                       key[NGX_HTTP_CACHE_KEY_LEN];

    rc = ngx_http_cache_purge_soft_header(path, log);
    if (rc != NGX_OK) {
        return rc;
    }

    if (ngx_http_cache_purge_filename_key(path, key) == NGX_OK) {
        ngx_shmtx_lock(&cache->shpool->mutex);

        node = ngx_http_cache_purge_lookup(cache, key);
        if (node != NULL && node->exists) {
            node->valid_sec = 0;
        }

        ngx_shmtx_unlock(&cache->shpool->mutex);
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_purge_access_handler(ngx_http_request_t *r) {
    ngx_http_cache_purge_loc_conf_t   *cplcf;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /* Safety check: if conf is not properly initialized, fall through to original handler */
    if (cplcf->conf == NULL || cplcf->conf == NGX_CONF_UNSET_PTR) {
        if (cplcf->original_handler) {
            return cplcf->original_handler(r);
        }
        return NGX_DECLINED;
    }

    if (r->method_name.len != cplcf->conf->method.len
            || (ngx_strncmp(r->method_name.data, cplcf->conf->method.data,
                            r->method_name.len))) {
        return cplcf->original_handler(r);
    }

    if ((cplcf->conf->access || cplcf->conf->access6)
            && ngx_http_cache_purge_access(cplcf->conf->access,
                                           cplcf->conf->access6,
                                           r->connection->sockaddr) != NGX_OK) {
        return NGX_HTTP_FORBIDDEN;
    }

    if (cplcf->handler == NULL) {
        return NGX_HTTP_NOT_FOUND;
    }

    return cplcf->handler(r);
}

ngx_int_t
ngx_http_cache_purge_access(ngx_array_t *access, ngx_array_t *access6,
                            struct sockaddr *s) {
    in_addr_t         inaddr;
    ngx_in_cidr_t    *a;
    ngx_uint_t        i;
# if (NGX_HAVE_INET6)
    struct in6_addr  *inaddr6;
    ngx_in6_cidr_t   *a6;
    u_char           *p;
    ngx_uint_t        n;
# endif /* NGX_HAVE_INET6 */

    switch (s->sa_family) {
    case AF_INET:
        if (access == NULL) {
            return NGX_DECLINED;
        }

        inaddr = ((struct sockaddr_in *) s)->sin_addr.s_addr;

# if (NGX_HAVE_INET6)
ipv4:
# endif /* NGX_HAVE_INET6 */

        a = access->elts;
        for (i = 0; i < access->nelts; i++) {
            if ((inaddr & a[i].mask) == a[i].addr) {
                return NGX_OK;
            }
        }

        return NGX_DECLINED;

# if (NGX_HAVE_INET6)
    case AF_INET6:
        inaddr6 = &((struct sockaddr_in6 *) s)->sin6_addr;
        p = inaddr6->s6_addr;

        if (access && IN6_IS_ADDR_V4MAPPED(inaddr6)) {
            inaddr = p[12] << 24;
            inaddr += p[13] << 16;
            inaddr += p[14] << 8;
            inaddr += p[15];
            inaddr = htonl(inaddr);

            goto ipv4;
        }

        if (access6 == NULL) {
            return NGX_DECLINED;
        }

        a6 = access6->elts;
        for (i = 0; i < access6->nelts; i++) {
            for (n = 0; n < 16; n++) {
                if ((p[n] & a6[i].mask.s6_addr[n]) != a6[i].addr.s6_addr[n]) {
                    goto next;
                }
            }

            return NGX_OK;

next:
            continue;
        }

        return NGX_DECLINED;
# endif /* NGX_HAVE_INET6 */
    }

    return NGX_DECLINED;
}

ngx_int_t
ngx_http_cache_purge_send_response(ngx_http_request_t *r) {
    ngx_chain_t   out;
    ngx_buf_t    *b;
    ngx_str_t    *key;
    ngx_int_t     rc;
    size_t        len;

    size_t body_len;
    size_t resp_tmpl_len;
    u_char *buf;
    u_char *buf_keydata;
    u_char *p;
    const char *resp_ct;
    size_t resp_ct_size;
    const char *resp_body;
    size_t resp_body_size;

    ngx_http_cache_purge_loc_conf_t   *cplcf;
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    key = r->cache->keys.elts;

    buf_keydata = ngx_pcalloc(r->pool, key[0].len+1);
    if (buf_keydata == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_cpymem(buf_keydata, key[0].data, key[0].len);
    if (p == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    switch (cplcf->resptype) {

    case NGX_REPONSE_TYPE_JSON:
        resp_ct = ngx_http_cache_purge_content_type_json;
        resp_ct_size = ngx_http_cache_purge_content_type_json_size;
        resp_body = ngx_http_cache_purge_body_templ_json;
        resp_body_size = ngx_http_cache_purge_body_templ_json_size;
        break;

    case NGX_REPONSE_TYPE_XML:
        resp_ct = ngx_http_cache_purge_content_type_xml;
        resp_ct_size = ngx_http_cache_purge_content_type_xml_size;
        resp_body = ngx_http_cache_purge_body_templ_xml;
        resp_body_size = ngx_http_cache_purge_body_templ_xml_size;
        break;

    case NGX_REPONSE_TYPE_TEXT:
        resp_ct = ngx_http_cache_purge_content_type_text;
        resp_ct_size = ngx_http_cache_purge_content_type_text_size;
        resp_body = ngx_http_cache_purge_body_templ_text;
        resp_body_size = ngx_http_cache_purge_body_templ_text_size;
        break;

    default:
    case NGX_REPONSE_TYPE_HTML:
        resp_ct = ngx_http_cache_purge_content_type_html;
        resp_ct_size = ngx_http_cache_purge_content_type_html_size;
        resp_body = ngx_http_cache_purge_body_templ_html;
        resp_body_size = ngx_http_cache_purge_body_templ_html_size;
        break;
    }

    body_len = resp_body_size - 2 - 1;
    r->headers_out.content_type.len = resp_ct_size - 1;
    r->headers_out.content_type.data = (u_char *) resp_ct;

    resp_tmpl_len = body_len + key[0].len ;

    buf = ngx_pcalloc(r->pool, resp_tmpl_len);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_snprintf(buf, resp_tmpl_len, resp_body, buf_keydata);
    if (p == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    len = body_len + key[0].len;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = len;

    if (r->method == NGX_HTTP_HEAD) {
        rc = ngx_http_send_header(r);
        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }


    out.buf = b;
    out.next = NULL;

    b->last = ngx_cpymem(b->last, buf, resp_tmpl_len);
    b->last_buf = 1;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

ngx_flag_t
ngx_http_cache_tag_location_enabled(ngx_http_cache_purge_loc_conf_t *cplcf) {
    return cplcf->cache_tag_watch && cplcf->cache_tag_headers != NULL;
}

ngx_int_t
ngx_http_cache_tag_request_headers(ngx_http_request_t *r, ngx_array_t **tags) {
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_list_part_t                  *part;
    ngx_table_elt_t                  *header;
    ngx_str_t                        *wanted;
    ngx_uint_t                        i, j;
    ngx_array_t                      *result;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    if (!ngx_http_cache_tag_location_enabled(cplcf)) {
        *tags = NULL;
        return NGX_DECLINED;
    }

    result = ngx_array_create(r->pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    part = &r->headers_in.headers.part;
    header = part->elts;
    wanted = cplcf->cache_tag_headers->elts;

    for (i = 0; ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        for (j = 0; j < cplcf->cache_tag_headers->nelts; j++) {
            if (header[i].key.len != wanted[j].len) {
                continue;
            }

            if (ngx_strncasecmp(header[i].key.data, wanted[j].data,
                                wanted[j].len) != 0) {
                continue;
            }

            if (ngx_http_cache_tag_extract_tokens(r->pool, header[i].value.data,
                                                  header[i].value.len, result)
                    != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    *tags = result;

    return result->nelts > 0 ? NGX_OK : NGX_DECLINED;
}

ngx_int_t
ngx_http_cache_purge_by_path(ngx_http_file_cache_t *cache, ngx_str_t *path,
                             ngx_flag_t soft, ngx_log_t *log) {
    ngx_http_file_cache_node_t  *node;
    ngx_file_info_t              fi;
    u_char                       key[NGX_HTTP_CACHE_KEY_LEN];

    if (soft) {
        return ngx_http_cache_purge_soft_path(cache, path, log);
    }

    if (ngx_file_info(path->data, &fi) == NGX_FILE_ERROR) {
        return ngx_errno == NGX_ENOENT ? NGX_DECLINED : NGX_ERROR;
    }

    if (ngx_http_cache_purge_filename_key(path, key) == NGX_OK) {
        ngx_shmtx_lock(&cache->shpool->mutex);

        node = ngx_http_cache_purge_lookup(cache, key);
        if (node != NULL && node->exists) {
#  if (nginx_version >= 1000001)
            cache->sh->size -= node->fs_size;
            node->fs_size = 0;
#  else
            cache->sh->size -= (node->length + cache->bsize - 1) / cache->bsize;
            node->length = 0;
#  endif
            node->exists = 0;
#  if (nginx_version >= 8001) \
       || ((nginx_version < 8000) && (nginx_version >= 7060))
            node->updating = 0;
#  endif
        }

        ngx_shmtx_unlock(&cache->shpool->mutex);
    }

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        if (ngx_errno == NGX_ENOENT) {
            return NGX_DECLINED;
        }

        ngx_log_error(NGX_LOG_CRIT, log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", path->data);
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_register_cache(ngx_conf_t *cf, ngx_http_file_cache_t *cache) {
    ngx_http_cache_purge_main_conf_t  *pmcf;
    ngx_http_cache_tag_zone_t         *zones, *zone;
    ngx_uint_t                         i;

    if (cache == NULL) {
        return NGX_OK;
    }

    pmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);
    if (pmcf == NULL || pmcf->sqlite_path.len == 0) {
        return NGX_OK;
    }

    zones = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        if (zones[i].cache == cache) {
            return NGX_OK;
        }
    }

    zone = ngx_array_push(pmcf->zones);
    if (zone == NULL) {
        return NGX_ERROR;
    }

    zone->cache = cache;
    zone->zone_name = cache->shm_zone->shm.name;

    return NGX_OK;
}

#if (NGX_LINUX)
static sqlite3 *
ngx_http_cache_tag_db_open(ngx_str_t *path, int flags, ngx_log_t *log) {
    sqlite3  *db;
    int       rc;

    db = NULL;
    rc = sqlite3_open_v2((const char *) path->data, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite open failed for \"%V\": %s", path,
                      db != NULL ? sqlite3_errmsg(db) : "unknown");
        if (db != NULL) {
            sqlite3_close(db);
        }
        return NULL;
    }

    return db;
}

static ngx_int_t
ngx_http_cache_tag_sqlite_init(sqlite3 *db, ngx_log_t *log) {
    static const char *schema[] = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=NORMAL;",
        "CREATE TABLE IF NOT EXISTS cache_tag_entries ("
        " zone TEXT NOT NULL,"
        " tag TEXT NOT NULL,"
        " path TEXT NOT NULL,"
        " mtime INTEGER NOT NULL,"
        " size INTEGER NOT NULL,"
        " PRIMARY KEY(zone, tag, path)"
        ");",
        "CREATE INDEX IF NOT EXISTS cache_tag_entries_lookup "
        "ON cache_tag_entries(zone, tag);"
    };
    char        *errmsg;
    ngx_uint_t   i;

    for (i = 0; i < sizeof(schema) / sizeof(schema[0]); i++) {
        errmsg = NULL;
        if (sqlite3_exec(db, schema[i], NULL, NULL, &errmsg) != SQLITE_OK) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite schema init failed: %s",
                          errmsg != NULL ? errmsg : "unknown");
            if (errmsg != NULL) {
                sqlite3_free(errmsg);
            }
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_db_delete_file(sqlite3 *db, ngx_str_t *zone_name,
                                  ngx_str_t *path, ngx_log_t *log) {
    sqlite3_stmt  *stmt;
    int            rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db,
                            "DELETE FROM cache_tag_entries "
                            "WHERE zone = ?1 AND path = ?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite prepare delete failed: %s",
                      sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    sqlite3_bind_text(stmt, 1, (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, (const char *) path->data, path->len,
                      SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite delete failed: %s", sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_db_replace(sqlite3 *db, ngx_str_t *zone_name, ngx_str_t *path,
                              time_t mtime, off_t size, ngx_array_t *tags,
                              ngx_log_t *log) {
    sqlite3_stmt  *stmt;
    ngx_str_t     *tag;
    ngx_uint_t     i;
    int            rc;

    if (ngx_http_cache_tag_db_delete_file(db, zone_name, path, log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (tags == NULL || tags->nelts == 0) {
        return NGX_OK;
    }

    rc = sqlite3_prepare_v2(db,
                            "INSERT OR REPLACE INTO cache_tag_entries "
                            "(zone, tag, path, mtime, size) "
                            "VALUES (?1, ?2, ?3, ?4, ?5)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite prepare insert failed: %s",
                      sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, (const char *) zone_name->data, zone_name->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, (const char *) tag[i].data, tag[i].len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, (const char *) path->data, path->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64) mtime);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64) size);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite insert failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return NGX_ERROR;
        }
    }

    sqlite3_finalize(stmt);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_db_collect_paths(sqlite3 *db, ngx_pool_t *pool,
                                    ngx_str_t *zone_name, ngx_array_t *tags,
                                    ngx_array_t **paths, ngx_log_t *log) {
    sqlite3_stmt  *stmt;
    ngx_array_t   *result;
    ngx_str_t     *tag;
    const u_char  *text;
    ngx_str_t     *path;
    ngx_uint_t     i, j;
    int            rc;
    size_t         len;

    result = ngx_array_create(pool, 8, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    rc = sqlite3_prepare_v2(db,
                            "SELECT path FROM cache_tag_entries "
                            "WHERE zone = ?1 AND tag = ?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite prepare lookup failed: %s", sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, (const char *) zone_name->data, zone_name->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, (const char *) tag[i].data, tag[i].len,
                          SQLITE_TRANSIENT);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            text = sqlite3_column_text(stmt, 0);
            if (text == NULL) {
                continue;
            }

            len = ngx_strlen(text);
            path = result->elts;
            for (j = 0; j < result->nelts; j++) {
                if (path[j].len == len
                        && ngx_strncmp(path[j].data, text, len) == 0) {
                    break;
                }
            }

            if (j != result->nelts) {
                continue;
            }

            path = ngx_array_push(result);
            if (path == NULL) {
                sqlite3_finalize(stmt);
                return NGX_ERROR;
            }

            path->data = ngx_pnalloc(pool, len + 1);
            if (path->data == NULL) {
                sqlite3_finalize(stmt);
                return NGX_ERROR;
            }

            ngx_memcpy(path->data, text, len);
            path->len = len;
            path->data[len] = '\0';
        }

        if (rc != SQLITE_DONE) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite lookup failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return NGX_ERROR;
        }
    }

    sqlite3_finalize(stmt);
    *paths = result;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_push_unique(ngx_pool_t *pool, ngx_array_t *tags,
                               u_char *data, size_t len) {
    ngx_str_t   *tag;
    ngx_uint_t   i;

    if (len == 0) {
        return NGX_OK;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        if (tag[i].len == len
                && ngx_strncasecmp(tag[i].data, data, len) == 0) {
            return NGX_OK;
        }
    }

    tag = ngx_array_push(tags);
    if (tag == NULL) {
        return NGX_ERROR;
    }

    tag->data = ngx_pnalloc(pool, len);
    if (tag->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(tag->data, data, len);
    tag->len = len;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_extract_tokens(ngx_pool_t *pool, u_char *value, size_t len,
                                  ngx_array_t *tags) {
    size_t   i, start, end;

    i = 0;
    while (i < len) {
        while (i < len && (value[i] == ' ' || value[i] == '\t'
                           || value[i] == '\r' || value[i] == '\n'
                           || value[i] == ',')) {
            i++;
        }

        start = i;

        while (i < len && value[i] != ' ' && value[i] != '\t'
                && value[i] != '\r' && value[i] != '\n'
                && value[i] != ',') {
            i++;
        }

        end = i;
        if (end > start
                && ngx_http_cache_tag_push_unique(pool, tags, value + start,
                                                  end - start) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_join_path(ngx_pool_t *pool, ngx_str_t *base,
                             const char *name, ngx_str_t *out) {
    size_t  name_len;

    name_len = ngx_strlen(name);
    out->len = base->len + 1 + name_len;
    out->data = ngx_pnalloc(pool, out->len + 1);
    if (out->data == NULL) {
        return NGX_ERROR;
    }

    ngx_snprintf(out->data, out->len + 1, "%V/%s%Z", base, name);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_path_known(ngx_str_t *path) {
    u_char  *p;

    p = path->data + path->len;
    while (p > path->data && p[-1] != '/') {
        p--;
    }

    return (size_t) (path->data + path->len - p) == 2 * NGX_HTTP_CACHE_KEY_LEN;
}

static ngx_int_t
ngx_http_cache_tag_parse_file(ngx_pool_t *pool, ngx_str_t *path,
                              ngx_array_t *headers, ngx_array_t **tags,
                              time_t *mtime, off_t *size, ngx_log_t *log) {
    ngx_file_info_t  fi;
    ngx_file_t       file;
    u_char          *buf;
    ssize_t          n;
    size_t           max_read, i, j, line_end, value_start;
    ngx_array_t     *result;
    ngx_str_t       *header;

    if (!ngx_http_cache_tag_path_known(path)) {
        return NGX_DECLINED;
    }

    if (ngx_file_info(path->data, &fi) == NGX_FILE_ERROR) {
        return NGX_DECLINED;
    }

    max_read = (size_t) ngx_min((off_t) 65536, ngx_file_size(&fi));
    if (max_read == 0) {
        return NGX_DECLINED;
    }

    buf = ngx_pnalloc(pool, max_read);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = *path;
    file.log = log;
    file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (file.fd == NGX_INVALID_FILE) {
        return NGX_DECLINED;
    }

    n = ngx_read_file(&file, buf, max_read, 0);
    ngx_close_file(file.fd);

    if (n == NGX_ERROR || n <= 0) {
        return NGX_DECLINED;
    }

    result = ngx_array_create(pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    header = headers->elts;
    for (i = 0; i < (size_t) n; i++) {
        if (i != 0 && buf[i - 1] != '\n') {
            continue;
        }

        for (j = 0; j < headers->nelts; j++) {
            if (i + header[j].len + 1 >= (size_t) n) {
                continue;
            }

            if (ngx_strncasecmp(buf + i, header[j].data, header[j].len) != 0
                    || buf[i + header[j].len] != ':') {
                continue;
            }

            value_start = i + header[j].len + 1;
            while (value_start < (size_t) n
                    && (buf[value_start] == ' ' || buf[value_start] == '\t')) {
                value_start++;
            }

            line_end = value_start;
            while (line_end < (size_t) n && buf[line_end] != '\n'
                    && buf[line_end] != '\r') {
                line_end++;
            }

            if (ngx_http_cache_tag_extract_tokens(pool, buf + value_start,
                                                  line_end - value_start,
                                                  result) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    *mtime = ngx_file_mtime(&fi);
    *size = ngx_file_size(&fi);
    *tags = result;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_process_file(sqlite3 *db, ngx_str_t *zone_name, ngx_str_t *path,
                                ngx_log_t *log) {
    ngx_array_t  *headers;
    ngx_array_t  *tags;
    ngx_str_t    *header;
    time_t        mtime;
    off_t         size;
    ngx_int_t     rc;
    ngx_pool_t   *pool;

    pool = ngx_create_pool(4096, log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    headers = ngx_array_create(pool, 2, sizeof(ngx_str_t));
    if (headers == NULL) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    header = ngx_array_push(headers);
    if (header == NULL) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }
    ngx_str_set(header, "Surrogate-Key");

    header = ngx_array_push(headers);
    if (header == NULL) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }
    ngx_str_set(header, "Cache-Tag");

    if (ngx_http_cache_tag_parse_file(pool, path, headers,
                                      &tags, &mtime, &size, log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_tag_db_delete_file(db, zone_name, path, log);
    }

    rc = ngx_http_cache_tag_db_replace(db, zone_name, path, mtime, size,
                                       tags, log);
    ngx_destroy_pool(pool);

    return rc;
}

static ngx_http_cache_tag_watch_t *
ngx_http_cache_tag_find_watch(int wd) {
    ngx_http_cache_tag_watch_t  *watch;
    ngx_uint_t                   i;

    if (ngx_http_cache_tag_runtime.watches == NULL) {
        return NULL;
    }

    watch = ngx_http_cache_tag_runtime.watches->elts;
    for (i = 0; i < ngx_http_cache_tag_runtime.watches->nelts; i++) {
        if (watch[i].wd == wd) {
            return &watch[i];
        }
    }

    return NULL;
}

static ngx_int_t
ngx_http_cache_tag_remove_watch(int wd, ngx_cycle_t *cycle) {
    ngx_http_cache_tag_watch_t  *watch;
    ngx_uint_t                   i;

    if (ngx_http_cache_tag_runtime.watches == NULL) {
        return NGX_OK;
    }

    watch = ngx_http_cache_tag_runtime.watches->elts;
    for (i = 0; i < ngx_http_cache_tag_runtime.watches->nelts; i++) {
        if (watch[i].wd == wd) {
            inotify_rm_watch(ngx_http_cache_tag_runtime.inotify_fd, wd);
            if (i + 1 < ngx_http_cache_tag_runtime.watches->nelts) {
                watch[i] = watch[ngx_http_cache_tag_runtime.watches->nelts - 1];
            }
            ngx_http_cache_tag_runtime.watches->nelts--;
            return NGX_OK;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_add_watch(sqlite3 *db, ngx_http_cache_tag_zone_t *zone,
                             ngx_str_t *path, ngx_cycle_t *cycle) {
    ngx_http_cache_tag_watch_t  *watch;
    int                          wd;

    (void) db;

    if (ngx_http_cache_tag_runtime.watches == NULL) {
        ngx_http_cache_tag_runtime.watches = ngx_array_create(
            cycle->pool, 16, sizeof(ngx_http_cache_tag_watch_t));
        if (ngx_http_cache_tag_runtime.watches == NULL) {
            return NGX_ERROR;
        }
    }

    if (ngx_http_cache_tag_runtime.inotify_fd <= 0) {
        return NGX_DECLINED;
    }

    wd = inotify_add_watch(ngx_http_cache_tag_runtime.inotify_fd,
                           (const char *) path->data,
                           IN_CREATE|IN_MOVED_TO|IN_CLOSE_WRITE|IN_DELETE
                           |IN_MOVED_FROM|IN_DELETE_SELF|IN_ONLYDIR);
    if (wd == -1) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, ngx_errno,
                      "inotify_add_watch failed for \"%V\"", path);
        return NGX_DECLINED;
    }

    watch = ngx_http_cache_tag_find_watch(wd);
    if (watch != NULL) {
        return NGX_OK;
    }

    watch = ngx_array_push(ngx_http_cache_tag_runtime.watches);
    if (watch == NULL) {
        return NGX_ERROR;
    }

    watch->wd = wd;
    watch->cache = zone->cache;
    watch->zone_name = zone->zone_name;
    watch->path.len = path->len;
    watch->path.data = ngx_pnalloc(cycle->pool, path->len + 1);
    if (watch->path.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(watch->path.data, path->data, path->len);
    watch->path.data[path->len] = '\0';

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_add_watch_recursive(sqlite3 *db, ngx_http_cache_tag_zone_t *zone,
                                       ngx_str_t *path, ngx_cycle_t *cycle) {
    DIR            *dir;
    struct dirent  *entry;
    ngx_str_t       child;

    if (ngx_http_cache_tag_add_watch(db, zone, path, cycle) == NGX_ERROR) {
        return NGX_ERROR;
    }

    dir = opendir((const char *) path->data);
    if (dir == NULL) {
        return NGX_DECLINED;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (ngx_strcmp(entry->d_name, ".") == 0
                || ngx_strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (ngx_http_cache_tag_join_path(cycle->pool, path, entry->d_name, &child)
                != NGX_OK) {
            closedir(dir);
            return NGX_ERROR;
        }

        if (entry->d_type == DT_DIR) {
            if (ngx_http_cache_tag_add_watch_recursive(db, zone, &child, cycle)
                    != NGX_OK) {
                continue;
            }
            continue;
        }

        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            if (ngx_http_cache_tag_process_file(db, &zone->zone_name, &child,
                                                cycle->log)
                    != NGX_OK) {
                continue;
            }
        }
    }

    closedir(dir);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_process_events(ngx_cycle_t *cycle) {
    u_char                      buf[8192];
    ssize_t                     n;
    size_t                      offset;
    struct inotify_event       *event;
    ngx_http_cache_tag_watch_t *watch;
    ngx_str_t                   path;
    ngx_pool_t                 *pool;

    for ( ;; ) {
        n = read(ngx_http_cache_tag_runtime.inotify_fd, buf, sizeof(buf));
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return NGX_OK;
            }
            return NGX_ERROR;
        }

        if (n == 0) {
            return NGX_OK;
        }

        pool = ngx_create_pool(4096, cycle->log);
        if (pool == NULL) {
            return NGX_ERROR;
        }

        offset = 0;
        while (offset < (size_t) n) {
            event = (struct inotify_event *) (buf + offset);
            watch = ngx_http_cache_tag_find_watch(event->wd);
            if (watch != NULL) {
                if (event->mask & (IN_DELETE_SELF|IN_IGNORED)) {
                    ngx_http_cache_tag_remove_watch(event->wd, cycle);
                } else if (event->len > 0
                           && ngx_http_cache_tag_join_path(pool, &watch->path,
                                                           event->name, &path) == NGX_OK) {
                    if (event->mask & IN_ISDIR) {
                        if (event->mask & (IN_CREATE|IN_MOVED_TO)) {
                            ngx_http_cache_tag_zone_t zone;
                            zone.zone_name = watch->zone_name;
                            zone.cache = watch->cache;
                            ngx_http_cache_tag_add_watch_recursive(
                                ngx_http_cache_tag_runtime.db, &zone, &path, cycle);
                        }
                    } else if (event->mask & (IN_CREATE|IN_MOVED_TO|IN_CLOSE_WRITE)) {
                        ngx_http_cache_tag_process_file(ngx_http_cache_tag_runtime.db,
                                                        &watch->zone_name, &path,
                                                        cycle->log);
                    } else if (event->mask & (IN_DELETE|IN_MOVED_FROM)) {
                        ngx_http_cache_tag_db_delete_file(ngx_http_cache_tag_runtime.db,
                                                          &watch->zone_name, &path,
                                                          cycle->log);
                    }
                }
            }

            offset += sizeof(struct inotify_event) + event->len;
        }

        ngx_destroy_pool(pool);
    }
}

static void
ngx_http_cache_tag_timer_handler(ngx_event_t *ev) {
    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    if (ngx_http_cache_tag_process_events(ngx_http_cache_tag_runtime.cycle) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ngx_http_cache_tag_runtime.cycle->log, 0,
                      "cache_tag watcher processing failed");
    }

    ngx_add_timer(ev, 250);
}

static ngx_int_t
ngx_http_cache_tag_init_runtime(ngx_cycle_t *cycle,
                                ngx_http_cache_purge_main_conf_t *pmcf) {
    ngx_http_cache_tag_zone_t  *zone;
    ngx_uint_t                  i;

    ngx_memzero(&ngx_http_cache_tag_runtime, sizeof(ngx_http_cache_tag_runtime));

    ngx_http_cache_tag_runtime.owner = (ngx_process == NGX_PROCESS_WORKER && ngx_worker == 0);
    if (!ngx_http_cache_tag_runtime.owner || pmcf->zones->nelts == 0) {
        return NGX_OK;
    }

    ngx_http_cache_tag_runtime.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ngx_http_cache_tag_runtime.inotify_fd == -1) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, ngx_errno,
                      "inotify_init1 failed");
        return NGX_ERROR;
    }

    ngx_http_cache_tag_runtime.db = ngx_http_cache_tag_db_open(
        &pmcf->sqlite_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, cycle->log);
    if (ngx_http_cache_tag_runtime.db == NULL) {
        close(ngx_http_cache_tag_runtime.inotify_fd);
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_sqlite_init(ngx_http_cache_tag_runtime.db, cycle->log)
            != NGX_OK) {
        ngx_http_cache_tag_shutdown_runtime();
        return NGX_ERROR;
    }

    ngx_http_cache_tag_runtime.watches = ngx_array_create(cycle->pool, 16,
                                                          sizeof(ngx_http_cache_tag_watch_t));
    if (ngx_http_cache_tag_runtime.watches == NULL) {
        ngx_http_cache_tag_shutdown_runtime();
        return NGX_ERROR;
    }

    zone = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        if (zone[i].cache == NULL || zone[i].cache->path == NULL) {
            continue;
        }

        if (ngx_http_cache_tag_add_watch_recursive(ngx_http_cache_tag_runtime.db,
                                                   &zone[i], &zone[i].cache->path->name,
                                                   cycle) == NGX_ERROR) {
            ngx_http_cache_tag_shutdown_runtime();
            return NGX_ERROR;
        }
    }

    ngx_http_cache_tag_runtime.cycle = cycle;
    ngx_http_cache_tag_runtime.timer.handler = ngx_http_cache_tag_timer_handler;
    ngx_http_cache_tag_runtime.timer.log = cycle->log;
    ngx_http_cache_tag_runtime.timer.data = NULL;
    ngx_http_cache_tag_runtime.timer.cancelable = 1;
    ngx_add_timer(&ngx_http_cache_tag_runtime.timer, 250);

    ngx_http_cache_tag_runtime.initialized = 1;
    ngx_http_cache_tag_runtime.active = 1;

    return NGX_OK;
}

static void
ngx_http_cache_tag_shutdown_runtime(void) {
    if (ngx_http_cache_tag_runtime.timer.timer_set) {
        ngx_del_timer(&ngx_http_cache_tag_runtime.timer);
    }

    if (ngx_http_cache_tag_runtime.db != NULL) {
        sqlite3_close(ngx_http_cache_tag_runtime.db);
    }

    if (ngx_http_cache_tag_runtime.inotify_fd > 0) {
        close(ngx_http_cache_tag_runtime.inotify_fd);
    }

    ngx_memzero(&ngx_http_cache_tag_runtime, sizeof(ngx_http_cache_tag_runtime));
}

static ngx_http_cache_tag_zone_t *
ngx_http_cache_tag_lookup_zone(ngx_http_file_cache_t *cache) {
    ngx_http_conf_ctx_t              *http_ctx;
    ngx_http_cache_purge_main_conf_t *pmcf;
    ngx_http_cache_tag_zone_t        *zone;
    ngx_uint_t                        i;

    http_ctx = (ngx_http_conf_ctx_t *) ngx_get_conf(ngx_cycle->conf_ctx,
               ngx_http_module);
    pmcf = http_ctx->main_conf[ngx_http_cache_purge_module.ctx_index];
    if (pmcf == NULL || pmcf->zones == NULL) {
        return NULL;
    }

    zone = pmcf->zones->elts;
    for (i = 0; i < pmcf->zones->nelts; i++) {
        if (zone[i].cache == cache) {
            return &zone[i];
        }
    }

    return NULL;
}
#endif

# if (nginx_version >= 1007009)

/*
 * Based on: ngx_http_upstream.c/ngx_http_upstream_cache_get
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */
ngx_int_t
ngx_http_cache_purge_cache_get(ngx_http_request_t *r, ngx_http_upstream_t *u,
                               ngx_http_file_cache_t **cache) {
    ngx_str_t               *name, val;
    ngx_uint_t               i;
    ngx_http_file_cache_t  **caches;

    if (u->conf->cache_zone) {
        *cache = u->conf->cache_zone->data;
        return NGX_OK;
    }

    if (ngx_http_complex_value(r, u->conf->cache_value, &val) != NGX_OK) {
        return NGX_ERROR;
    }

    if (val.len == 0
            || (val.len == 3 && ngx_strncmp(val.data, "off", 3) == 0)) {
        return NGX_DECLINED;
    }

    caches = u->caches->elts;

    for (i = 0; i < u->caches->nelts; i++) {
        name = &caches[i]->shm_zone->shm.name;

        if (name->len == val.len
                && ngx_strncmp(name->data, val.data, val.len) == 0) {
            *cache = caches[i];
            return NGX_OK;
        }
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "cache \"%V\" not found", &val);

    return NGX_ERROR;
}

# endif /* nginx_version >= 1007009 */

ngx_int_t
ngx_http_cache_tag_purge(ngx_http_request_t *r, ngx_http_file_cache_t *cache) {
    ngx_http_conf_ctx_t              *http_ctx;
    ngx_http_cache_purge_main_conf_t *pmcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_tag_zone_t        *zone;
    ngx_array_t                      *tags, *paths;
    ngx_str_t                        *path;
    ngx_uint_t                        i;
    ngx_int_t                         rc, purged;
#if (NGX_LINUX)
    sqlite3                          *db;
#endif

    rc = ngx_http_cache_tag_request_headers(r, &tags);
    if (rc != NGX_OK) {
        return NGX_DECLINED;
    }

    http_ctx = (ngx_http_conf_ctx_t *) ngx_get_conf(ngx_cycle->conf_ctx,
               ngx_http_module);
    pmcf = http_ctx->main_conf[ngx_http_cache_purge_module.ctx_index];
    if (pmcf == NULL || pmcf->sqlite_path.len == 0) {
        return NGX_DECLINED;
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    zone = NULL;
#if (NGX_LINUX)
    zone = ngx_http_cache_tag_lookup_zone(cache);
#endif
    if (zone == NULL) {
        zone = ngx_pcalloc(r->pool, sizeof(ngx_http_cache_tag_zone_t));
        if (zone == NULL) {
            return NGX_ERROR;
        }
        zone->cache = cache;
        zone->zone_name = cache->shm_zone->shm.name;
    }

#if !(NGX_LINUX)
    return NGX_DECLINED;
#else
    db = ngx_http_cache_tag_db_open(&pmcf->sqlite_path,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                    r->connection->log);
    if (db == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_sqlite_init(db, r->connection->log) != NGX_OK) {
        sqlite3_close(db);
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_db_collect_paths(db, r->pool, &zone->zone_name,
                                            tags, &paths, r->connection->log)
            != NGX_OK) {
        sqlite3_close(db);
        return NGX_ERROR;
    }

    if (paths->nelts == 0 && cache->path != NULL) {
        ngx_http_cache_tag_add_watch_recursive(db, zone, &cache->path->name,
                                               (ngx_cycle_t *) ngx_cycle);
        if (ngx_http_cache_tag_db_collect_paths(db, r->pool, &zone->zone_name,
                                                tags, &paths, r->connection->log)
                != NGX_OK) {
            sqlite3_close(db);
            return NGX_ERROR;
        }
    }

    purged = 0;
    path = paths->elts;
    for (i = 0; i < paths->nelts; i++) {
        rc = ngx_http_cache_purge_by_path(cache, &path[i], cplcf->conf->soft,
                                          r->connection->log);
        if (rc == NGX_OK) {
            purged++;
            ngx_http_cache_tag_db_delete_file(db, &zone->zone_name, &path[i],
                                              r->connection->log);
        } else if (rc == NGX_DECLINED) {
            ngx_http_cache_tag_db_delete_file(db, &zone->zone_name, &path[i],
                                              r->connection->log);
        } else {
            sqlite3_close(db);
            return NGX_ERROR;
        }
    }

    sqlite3_close(db);

    return purged > 0 ? NGX_OK : NGX_DECLINED;
#endif
}

ngx_int_t
ngx_http_cache_purge_init(ngx_http_request_t *r, ngx_http_file_cache_t *cache,
                          ngx_http_complex_value_t *cache_key) {
    ngx_http_cache_t  *c;
    ngx_str_t         *key;
    ngx_int_t          rc;

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    c = ngx_pcalloc(r->pool, sizeof(ngx_http_cache_t));
    if (c == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_array_init(&c->keys, r->pool, 1, sizeof(ngx_str_t));
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    key = ngx_array_push(&c->keys);
    if (key == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_http_complex_value(r, cache_key, key);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    r->cache = c;
    c->body_start = ngx_pagesize;
    c->file_cache = cache;
    c->file.log = r->connection->log;

    ngx_http_file_cache_create_key(r);

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_purge_init_process(ngx_cycle_t *cycle) {
    ngx_http_conf_ctx_t              *http_ctx;
    ngx_http_cache_purge_main_conf_t *pmcf;

#if !(NGX_LINUX)
    return NGX_OK;
#else
    http_ctx = (ngx_http_conf_ctx_t *) ngx_get_conf(cycle->conf_ctx,
               ngx_http_module);
    if (http_ctx == NULL) {
        return NGX_OK;
    }

    pmcf = http_ctx->main_conf[ngx_http_cache_purge_module.ctx_index];
    if (pmcf == NULL || pmcf->sqlite_path.len == 0) {
        return NGX_OK;
    }

    return ngx_http_cache_tag_init_runtime(cycle, pmcf);
#endif
}

void
ngx_http_cache_purge_exit_process(ngx_cycle_t *cycle) {
    (void) cycle;
#if (NGX_LINUX)
    ngx_http_cache_tag_shutdown_runtime();
#endif
}

void
ngx_http_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_int_t  rc;
    ngx_array_t *tags;

#  if (NGX_HAVE_FILE_AIO)
    if (r->aio) {
        return;
    }
#  endif

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    rc = NGX_OK;
    tags = NULL;

    if (ngx_http_cache_tag_location_enabled(cplcf)
            && ngx_http_cache_tag_request_headers(r, &tags) == NGX_OK
            && tags != NULL
            && tags->nelts > 0) {
        rc = NGX_OK;
    } else if (!cplcf->conf->purge_all && !ngx_http_cache_purge_is_partial(r)) {
        if (cplcf->conf->soft) {
            rc = ngx_http_file_cache_purge_soft(r);
        } else {
            rc = ngx_http_file_cache_purge(r);
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http file cache purge: %i, \"%s\"",
                       rc, r->cache->file.name.data);
    }

    switch (rc) {
    case NGX_OK:
        r->write_event_handler = ngx_http_request_empty_handler;
        ngx_http_finalize_request(r, ngx_http_cache_purge_send_response(r));
        return;
    case NGX_DECLINED:
        ngx_http_finalize_request(r, NGX_HTTP_PRECONDITION_FAILED);
        return;
#  if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        r->write_event_handler = ngx_http_cache_purge_handler;
        return;
#  endif
    default:
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
    }
}

ngx_int_t
ngx_http_file_cache_purge(ngx_http_request_t *r) {
    ngx_http_file_cache_t  *cache;
    ngx_http_cache_t       *c;

    switch (ngx_http_file_cache_open(r)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
#  if (nginx_version >= 8001) \
       || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
#  endif
        break;
    case NGX_DECLINED:
        return NGX_DECLINED;
#  if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        return NGX_AGAIN;
#  endif
    default:
        return NGX_ERROR;
    }

    c = r->cache;
    cache = c->file_cache;

    /*
     * delete file from disk but *keep* in-memory node,
     * because other requests might still point to it.
     */

    ngx_shmtx_lock(&cache->shpool->mutex);

    if (!c->node->exists) {
        /* race between concurrent purges, backoff */
        ngx_shmtx_unlock(&cache->shpool->mutex);
        return NGX_DECLINED;
    }

#  if (nginx_version >= 1000001)
    cache->sh->size -= c->node->fs_size;
    c->node->fs_size = 0;
#  else
    cache->sh->size -= (c->node->length + cache->bsize - 1) / cache->bsize;
    c->node->length = 0;
#  endif

    c->node->exists = 0;
#  if (nginx_version >= 8001) \
       || ((nginx_version < 8000) && (nginx_version >= 7060))
    c->node->updating = 0;
#  endif

    ngx_shmtx_unlock(&cache->shpool->mutex);

    if (ngx_delete_file(c->file.name.data) == NGX_FILE_ERROR) {
        /* entry in error log is enough, don't notice client */
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", c->file.name.data);
    }

    /* file deleted from cache */
    return NGX_OK;
}

ngx_int_t
ngx_http_file_cache_purge_soft(ngx_http_request_t *r) {
    ngx_int_t              rc;
    ngx_http_file_cache_t  *cache;
    ngx_http_cache_t       *c;

    switch (ngx_http_file_cache_open(r)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
#  if (nginx_version >= 8001) \
       || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
#  endif
        break;
    case NGX_DECLINED:
        return NGX_DECLINED;
#  if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        return NGX_AGAIN;
#  endif
    default:
        return NGX_ERROR;
    }

    c = r->cache;
    cache = c->file_cache;

    ngx_shmtx_lock(&cache->shpool->mutex);

    if (!c->node->exists) {
        ngx_shmtx_unlock(&cache->shpool->mutex);
        return NGX_DECLINED;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);

    rc = ngx_http_cache_purge_soft_header(&c->file.name, r->connection->log);
    if (rc != NGX_OK) {
        return rc;
    }

    ngx_shmtx_lock(&cache->shpool->mutex);

    if (c->node->exists) {
        c->valid_sec = 0;
        c->node->valid_sec = 0;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);

    return NGX_OK;
}


ngx_int_t
ngx_http_cache_purge_all(ngx_http_request_t *r, ngx_http_file_cache_t *cache) {
    ngx_http_cache_purge_loc_conf_t    *cplcf;
    ngx_http_cache_purge_partial_ctx_t  ctx;

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "purge_all http in %s",
                  cache->path->name.data);

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    ngx_memzero(&ctx, sizeof(ctx));
    ctx.cache = cache;

    /* Walk the tree and remove all the files */
    ngx_tree_ctx_t  tree;
    tree.init_handler = NULL;
    tree.file_handler = cplcf->conf->soft
                        ? ngx_http_purge_file_cache_soft_file
                        : ngx_http_purge_file_cache_delete_file;
    tree.pre_tree_handler = ngx_http_purge_file_cache_noop;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler = ngx_http_purge_file_cache_noop;
    tree.data = &ctx;
    tree.alloc = 0;
    tree.log = ngx_cycle->log;

    if (ngx_walk_tree(&tree, &cache->path->name) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_purge_partial(ngx_http_request_t *r, ngx_http_file_cache_t *cache) {
    ngx_http_cache_purge_loc_conf_t    *cplcf;
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "purge_partial http in %s",
                  cache->path->name.data);

    ngx_str_t           *keys;
    ngx_str_t           key;

    /* Only check the first key, and discard '*' at the end */
    keys = r->cache->keys.elts;
    key = keys[0];
    key.len--;

    ngx_http_cache_purge_partial_ctx_t *ctx;
    ctx = ngx_palloc(r->pool, sizeof(ngx_http_cache_purge_partial_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(ctx, sizeof(ngx_http_cache_purge_partial_ctx_t));
    ctx->cache = cache;
    ctx->key_len = key.len;
    if (key.len > 0) {
        ctx->key_partial = key.data;
        ctx->key_in_file = ngx_pnalloc(r->pool, sizeof(u_char) * key.len);
        if (ctx->key_in_file == NULL) {
            return NGX_ERROR;
        }
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /* Walk the tree and remove all the files matching key_partial */
    ngx_tree_ctx_t  tree;
    tree.init_handler = NULL;
    tree.file_handler = cplcf->conf->soft
                        ? ngx_http_purge_file_cache_soft_partial_file
                        : ngx_http_purge_file_cache_delete_partial_file;
    tree.pre_tree_handler = ngx_http_purge_file_cache_noop;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler = ngx_http_purge_file_cache_noop;
    tree.data = ctx;
    tree.alloc = 0;
    tree.log = ngx_cycle->log;

    if (ngx_walk_tree(&tree, &cache->path->name) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_purge_is_partial(ngx_http_request_t *r) {
    ngx_str_t *key;
    ngx_http_cache_t  *c;

    c = r->cache;
    key = c->keys.elts;

    /* Only check the first key */
    return c->keys.nelts > 0
           && key[0].len > 0
           && key[0].data[key[0].len - 1] == '*';
}

char *
ngx_http_cache_purge_conf(ngx_conf_t *cf, ngx_http_cache_purge_conf_t *cpcf) {
    ngx_cidr_t       cidr;
    ngx_in_cidr_t   *access;
# if (NGX_HAVE_INET6)
    ngx_in6_cidr_t  *access6;
# endif /* NGX_HAVE_INET6 */
    ngx_str_t       *value;
    ngx_int_t        rc;
    ngx_uint_t       i;
    ngx_uint_t       from_position;

    from_position = 2;

    /* xxx_cache_purge on|off|<method> [soft] [purge_all] [from all|<ip> [.. <ip>]] */
    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "off") == 0) {
        cpcf->enable = 0;
        return NGX_CONF_OK;

    } else if (ngx_strcmp(value[1].data, "on") == 0) {
        ngx_str_set(&cpcf->method, "PURGE");

    } else {
        cpcf->method = value[1];
    }

    if (cf->args->nelts < 4) {
        cpcf->enable = 1;
        return NGX_CONF_OK;
    }

    if (ngx_strcmp(value[from_position].data, "soft") == 0) {
        cpcf->soft = 1;
        from_position++;
    }

    /* We will purge all the keys */
    if (ngx_strcmp(value[from_position].data, "purge_all") == 0) {
        cpcf->purge_all = 1;
        from_position++;
    }


    /* sanity check */
    if (ngx_strcmp(value[from_position].data, "from") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\", expected"
                           " \"from\" keyword", &value[from_position]);
        return NGX_CONF_ERROR;
    }

    if (ngx_strcmp(value[from_position + 1].data, "all") == 0) {
        cpcf->enable = 1;
        return NGX_CONF_OK;
    }

    for (i = (from_position + 1); i < cf->args->nelts; i++) {
        rc = ngx_ptocidr(&value[i], &cidr);

        if (rc == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid parameter \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "low address bits of %V are meaningless",
                               &value[i]);
        }

        switch (cidr.family) {
        case AF_INET:
            if (cpcf->access == NULL) {
                cpcf->access = ngx_array_create(cf->pool, cf->args->nelts - (from_position + 1),
                                                sizeof(ngx_in_cidr_t));
                if (cpcf->access == NULL) {
                    return NGX_CONF_ERROR;
                }
            }

            access = ngx_array_push(cpcf->access);
            if (access == NULL) {
                return NGX_CONF_ERROR;
            }

            access->mask = cidr.u.in.mask;
            access->addr = cidr.u.in.addr;

            break;

# if (NGX_HAVE_INET6)
        case AF_INET6:
            if (cpcf->access6 == NULL) {
                cpcf->access6 = ngx_array_create(cf->pool, cf->args->nelts - (from_position + 1),
                                                 sizeof(ngx_in6_cidr_t));
                if (cpcf->access6 == NULL) {
                    return NGX_CONF_ERROR;
                }
            }

            access6 = ngx_array_push(cpcf->access6);
            if (access6 == NULL) {
                return NGX_CONF_ERROR;
            }

            access6->mask = cidr.u.in6.mask;
            access6->addr = cidr.u.in6.addr;

            break;
# endif /* NGX_HAVE_INET6 */
        }
    }

    cpcf->enable = 1;

    return NGX_CONF_OK;
}

void
ngx_http_cache_purge_merge_conf(ngx_http_cache_purge_conf_t *conf,
                                ngx_http_cache_purge_conf_t *prev) {
    if (conf->enable == NGX_CONF_UNSET) {
        if (prev->enable == 1) {
            conf->enable = prev->enable;
            conf->method = prev->method;
            conf->soft = prev->soft;
            conf->purge_all = prev->purge_all;
            conf->access = prev->access;
            conf->access6 = prev->access6;
        } else {
            conf->enable = 0;
        }
    }
}

void *
ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf) {
    ngx_http_cache_purge_main_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_main_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->zones = ngx_array_create(cf->pool, 4, sizeof(ngx_http_cache_tag_zone_t));
    if (conf->zones == NULL) {
        return NULL;
    }

    return conf;
}

char *
ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf) {
    ngx_http_cache_purge_main_conf_t  *pmcf = conf;

    if (pmcf->sqlite_path.data == NULL) {
        pmcf->sqlite_path.len = 0;
    }

    return NGX_CONF_OK;
}

void *
ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_cache_purge_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->*.method = { 0, NULL }
     *     conf->*.access = NULL
     *     conf->*.access6 = NULL
     *     conf->handler = NULL
     *     conf->original_handler = NULL
     */

# if (NGX_HTTP_FASTCGI)
    conf->fastcgi.enable = NGX_CONF_UNSET;
# endif /* NGX_HTTP_FASTCGI */
# if (NGX_HTTP_PROXY)
    conf->proxy.enable = NGX_CONF_UNSET;
# endif /* NGX_HTTP_PROXY */
# if (NGX_HTTP_SCGI)
    conf->scgi.enable = NGX_CONF_UNSET;
# endif /* NGX_HTTP_SCGI */
# if (NGX_HTTP_UWSGI)
    conf->uwsgi.enable = NGX_CONF_UNSET;
# endif /* NGX_HTTP_UWSGI */

    conf->resptype = NGX_CONF_UNSET_UINT;
    conf->cache_tag_watch = NGX_CONF_UNSET;

    conf->conf = NGX_CONF_UNSET_PTR;

    return conf;
}

char *
ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_cache_purge_loc_conf_t  *prev = parent;
    ngx_http_cache_purge_loc_conf_t  *conf = child;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_str_t                        *header;
# if (NGX_HTTP_FASTCGI)
    ngx_http_fastcgi_loc_conf_t      *flcf;
# endif /* NGX_HTTP_FASTCGI */
# if (NGX_HTTP_PROXY)
    ngx_http_proxy_loc_conf_t        *plcf;
# endif /* NGX_HTTP_PROXY */
# if (NGX_HTTP_SCGI)
    ngx_http_scgi_loc_conf_t         *slcf;
# endif /* NGX_HTTP_SCGI */
# if (NGX_HTTP_UWSGI)
    ngx_http_uwsgi_loc_conf_t        *ulcf;
# endif /* NGX_HTTP_UWSGI */

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    ngx_conf_merge_uint_value(conf->resptype, prev->resptype, NGX_REPONSE_TYPE_HTML);
    ngx_conf_merge_value(conf->cache_tag_watch, prev->cache_tag_watch, 0);

    if (conf->cache_tag_headers == NULL) {
        conf->cache_tag_headers = prev->cache_tag_headers;
    }

    if (conf->cache_tag_headers == NULL) {
        conf->cache_tag_headers = ngx_array_create(cf->pool, 2, sizeof(ngx_str_t));
        if (conf->cache_tag_headers == NULL) {
            return NGX_CONF_ERROR;
        }

        header = ngx_array_push(conf->cache_tag_headers);
        if (header == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_str_set(header, "Surrogate-Key");

        header = ngx_array_push(conf->cache_tag_headers);
        if (header == NULL) {
            return NGX_CONF_ERROR;
        }
        ngx_str_set(header, "Cache-Tag");
    }

# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_merge_conf(&conf->fastcgi, &prev->fastcgi);

    if (conf->fastcgi.enable && clcf->handler != NULL) {
        flcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_fastcgi_module);

        if (flcf->upstream.upstream || flcf->fastcgi_lengths) {
            conf->conf = &conf->fastcgi;
            conf->handler = flcf->upstream.cache
                            ? ngx_http_fastcgi_cache_purge_handler : NULL;
            conf->original_handler = clcf->handler;

            clcf->handler = ngx_http_cache_purge_access_handler;

            if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                    flcf->upstream.cache_zone ? flcf->upstream.cache_zone->data : NULL
#  else
                    flcf->upstream.cache ? flcf->upstream.cache->data : NULL
#  endif
                                                                          ) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            return NGX_CONF_OK;
        }
    }
# endif /* NGX_HTTP_FASTCGI */

# if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_merge_conf(&conf->proxy, &prev->proxy);

    if (conf->proxy.enable && clcf->handler != NULL) {
        plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_proxy_module);

        if (plcf->upstream.upstream || plcf->proxy_lengths) {
            conf->conf = &conf->proxy;
            conf->handler = plcf->upstream.cache
                            ? ngx_http_proxy_cache_purge_handler : NULL;
            conf->original_handler = clcf->handler;

            clcf->handler = ngx_http_cache_purge_access_handler;

            if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                    plcf->upstream.cache_zone ? plcf->upstream.cache_zone->data : NULL
#  else
                    plcf->upstream.cache ? plcf->upstream.cache->data : NULL
#  endif
                                                                          ) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            return NGX_CONF_OK;
        }
    }
# endif /* NGX_HTTP_PROXY */

# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_merge_conf(&conf->scgi, &prev->scgi);

    if (conf->scgi.enable && clcf->handler != NULL) {
        slcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_scgi_module);

        if (slcf->upstream.upstream || slcf->scgi_lengths) {
            conf->conf = &conf->scgi;
            conf->handler = slcf->upstream.cache
                            ? ngx_http_scgi_cache_purge_handler : NULL;
            conf->original_handler = clcf->handler;
            clcf->handler = ngx_http_cache_purge_access_handler;

            if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                    slcf->upstream.cache_zone ? slcf->upstream.cache_zone->data : NULL
#  else
                    slcf->upstream.cache ? slcf->upstream.cache->data : NULL
#  endif
                                                                          ) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            return NGX_CONF_OK;
        }
    }
# endif /* NGX_HTTP_SCGI */

# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_merge_conf(&conf->uwsgi, &prev->uwsgi);

    if (conf->uwsgi.enable && clcf->handler != NULL) {
        ulcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_uwsgi_module);

        if (ulcf->upstream.upstream || ulcf->uwsgi_lengths) {
            conf->conf = &conf->uwsgi;
            conf->handler = ulcf->upstream.cache
                            ? ngx_http_uwsgi_cache_purge_handler : NULL;
            conf->original_handler = clcf->handler;

            clcf->handler = ngx_http_cache_purge_access_handler;

            if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                    ulcf->upstream.cache_zone ? ulcf->upstream.cache_zone->data : NULL
#  else
                    ulcf->upstream.cache ? ulcf->upstream.cache->data : NULL
#  endif
                                                                          ) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            return NGX_CONF_OK;
        }
    }
# endif /* NGX_HTTP_UWSGI */

    ngx_conf_merge_ptr_value(conf->conf, prev->conf, NULL);

    if (conf->handler == NULL) {
        conf->handler = prev->handler;
    }

    if (conf->original_handler == NULL) {
        conf->original_handler = prev->original_handler;
    }

    return NGX_CONF_OK;
}

#else /* !NGX_HTTP_CACHE */

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    NULL,  /* preconfiguration */
    NULL,  /* postconfiguration */

    NULL,  /* create main configuration */
    NULL,  /* init main configuration */

    NULL,  /* create server configuration */
    NULL,  /* merge server configuration */

    NULL,  /* create location configuration */
    NULL,  /* merge location configuration */
};

ngx_module_t  ngx_http_cache_purge_module = {
    NGX_MODULE_V1,
    &ngx_http_cache_purge_module_ctx,  /* module context */
    NULL,                              /* module directives */
    NGX_HTTP_MODULE,                   /* module type */
    NULL,                              /* init master */
    NULL,                              /* init module */
    NULL,                              /* init process */
    NULL,                              /* init thread */
    NULL,                              /* exit thread */
    NULL,                              /* exit process */
    NULL,                              /* exit master */
    NGX_MODULE_V1_PADDING
};

#endif /* NGX_HTTP_CACHE */
