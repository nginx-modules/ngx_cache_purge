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
#include "ngx_cache_purge_tag.h"


#ifndef nginx_version
    #error This module cannot be build against an unknown nginx version.
#endif

#define NGX_RESPONSE_TYPE_HTML 1
#define NGX_RESPONSE_TYPE_XML  2
#define NGX_RESPONSE_TYPE_JSON 3
#define NGX_RESPONSE_TYPE_TEXT 4

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

char        *ngx_http_cache_purge_mode_header_conf(ngx_conf_t *cf,
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
#if (NGX_CACHE_PURGE_THREADS)
static ngx_thread_pool_t *ngx_http_cache_purge_thread_pool(
    ngx_http_request_t *r);
static void ngx_http_cache_purge_partial_thread(void *data, ngx_log_t *log);
static void ngx_http_cache_purge_partial_completion(ngx_event_t *ev);
#endif

ngx_int_t   ngx_http_cache_purge_access_handler(ngx_http_request_t *r);
ngx_int_t   ngx_http_cache_purge_enabled(ngx_http_request_t *r,
        ngx_http_cache_purge_conf_t *cpcf);

ngx_int_t   ngx_http_cache_purge_send_response(ngx_http_request_t *r);
ngx_int_t   ngx_http_cache_purge_request_mode(ngx_http_request_t *r,
        ngx_flag_t default_soft);
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
static ngx_int_t ngx_http_cache_purge_dispatch_special(
    ngx_http_request_t *r, ngx_http_file_cache_t *cache,
    ngx_http_cache_purge_loc_conf_t *cplcf, ngx_flag_t *handled);

char       *ngx_http_cache_purge_conf(ngx_conf_t *cf,
                                      ngx_http_cache_purge_conf_t *cpcf);
static ngx_int_t ngx_http_cache_purge_complex_value_set(
    ngx_http_complex_value_t *cv);

void       *ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf);
char       *ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf);
void       *ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf);
char       *ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf,
        void *parent, void *child);
static ngx_int_t ngx_http_cache_purge_init_module(ngx_cycle_t *cycle);
ngx_int_t   ngx_http_cache_purge_init_process(ngx_cycle_t *cycle);
void        ngx_http_cache_purge_exit_process(ngx_cycle_t *cycle);

static ngx_conf_enum_t  ngx_http_cache_purge_response_types[] = {
    { ngx_string("html"), NGX_RESPONSE_TYPE_HTML },
    { ngx_string("xml"),  NGX_RESPONSE_TYPE_XML  },
    { ngx_string("json"), NGX_RESPONSE_TYPE_JSON },
    { ngx_string("text"), NGX_RESPONSE_TYPE_TEXT },
    { ngx_null_string,    0 }
};

static ngx_command_t  ngx_http_cache_purge_module_commands[] = {
    {
        ngx_string("cache_tag_index"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_2MORE,
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
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_cache_purge_loc_conf_t, cache_tag_watch),
        NULL
    },
    {
        ngx_string("cache_tag_queue_size"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_size_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_cache_purge_main_conf_t, queue_shm_size),
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
        ngx_conf_set_enum_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_cache_purge_loc_conf_t, resptype),
        &ngx_http_cache_purge_response_types
    },
    {
        ngx_string("cache_purge_mode_header"),
        NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
        ngx_http_cache_purge_mode_header_conf,
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
    ngx_http_cache_purge_init_module,      /* init module */
    ngx_http_cache_purge_init_process,     /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_http_cache_purge_exit_process,     /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t
ngx_http_cache_purge_dispatch_special(ngx_http_request_t *r,
                                      ngx_http_file_cache_t *cache,
                                      ngx_http_cache_purge_loc_conf_t *cplcf,
                                      ngx_flag_t *handled) {
    ngx_array_t  *tags;
    ngx_int_t     rc;

    *handled = 0;

    if (ngx_http_cache_tag_location_enabled(cplcf)) {
        tags = NULL;
        rc = ngx_http_cache_tag_request_headers(r, &tags);
        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (rc == NGX_OK && tags != NULL && tags->nelts > 0) {
            *handled = 1;
            return ngx_http_cache_tag_purge(r, cache, tags);
        }
    }

    if (cplcf->conf->purge_all) {
        *handled = 1;
        return ngx_http_cache_purge_all(r, cache);
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                      "http file cache purge with partial enabled");
        *handled = 1;
        return ngx_http_cache_purge_partial(r, cache);
    }

    return NGX_OK;
}

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
    ngx_http_cache_purge_loc_conf_t   *cplcf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->fastcgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }
    return ngx_http_cache_purge_conf(cf, &cplcf->fastcgi);
}

ngx_int_t
ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_fastcgi_loc_conf_t         *flcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_flag_t                           handled;
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
    handled = 0;
    rc = ngx_http_cache_purge_dispatch_special(r, cache, cplcf, &handled);
    if (handled) {
        if (rc == NGX_DONE) {
#  if (nginx_version >= 8011)
            r->main->count++;
#  endif
            return NGX_DONE;
        }

        if (rc == NGX_DECLINED) {
            return NGX_HTTP_PRECONDITION_FAILED;
        }

        if (rc != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
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
    ngx_http_cache_purge_loc_conf_t   *cplcf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->proxy.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    return ngx_http_cache_purge_conf(cf, &cplcf->proxy);
}

ngx_int_t
ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_proxy_loc_conf_t           *plcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_flag_t                           handled;
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
    handled = 0;
    rc = ngx_http_cache_purge_dispatch_special(r, cache, cplcf, &handled);
    if (handled) {
        if (rc == NGX_DONE) {
#  if (nginx_version >= 8011)
            r->main->count++;
#  endif
            return NGX_DONE;
        }

        if (rc == NGX_DECLINED) {
            return NGX_HTTP_PRECONDITION_FAILED;
        }

        if (rc != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
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
    ngx_http_cache_purge_loc_conf_t   *cplcf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->scgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    return ngx_http_cache_purge_conf(cf, &cplcf->scgi);
}

ngx_int_t
ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_scgi_loc_conf_t            *slcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_flag_t                           handled;
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
    handled = 0;
    rc = ngx_http_cache_purge_dispatch_special(r, cache, cplcf, &handled);
    if (handled) {
        if (rc == NGX_DONE) {
#  if (nginx_version >= 8011)
            r->main->count++;
#  endif
            return NGX_DONE;
        }

        if (rc == NGX_DECLINED) {
            return NGX_HTTP_PRECONDITION_FAILED;
        }

        if (rc != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
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
    ngx_http_cache_purge_loc_conf_t   *cplcf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    /* check for duplicates / collisions */
    if (cplcf->uwsgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    return ngx_http_cache_purge_conf(cf, &cplcf->uwsgi);
}


ngx_int_t
ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_file_cache_t               *cache;
    ngx_http_uwsgi_loc_conf_t           *ulcf;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_flag_t                           handled;
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
    handled = 0;
    rc = ngx_http_cache_purge_dispatch_special(r, cache, cplcf, &handled);
    if (handled) {
        if (rc == NGX_DONE) {
#  if (nginx_version >= 8011)
            r->main->count++;
#  endif
            return NGX_DONE;
        }

        if (rc == NGX_DECLINED) {
            return NGX_HTTP_PRECONDITION_FAILED;
        }

        if (rc != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

#  if (nginx_version >= 8011)
    r->main->count++;
#  endif

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_UWSGI */


char *
ngx_http_cache_purge_mode_header_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_str_t                         *value;

    (void) cmd;

    cplcf = conf;

    if (cplcf->purge_mode_header.data != NULL && cf->cmd_type == NGX_HTTP_LOC_CONF) {
        return "is duplicate";
    }

    value = cf->args->elts;
    cplcf->purge_mode_header = value[1];

    return NGX_CONF_OK;
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
        return NGX_ERROR;
    }

    return NGX_OK;
}


struct ngx_http_cache_purge_partial_ctx_s {
    ngx_http_file_cache_t *cache;
    u_char *key_partial;
    u_char *key_in_file;
    ngx_uint_t key_len;
};

#if (NGX_CACHE_PURGE_THREADS)
typedef struct {
    ngx_http_request_t                 *request;
    ngx_http_cache_purge_partial_ctx_t  partial;
    ngx_flag_t                          soft;
    ngx_int_t                           rc;
} ngx_http_cache_purge_partial_task_ctx_t;
#endif

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

#if (NGX_CACHE_PURGE_THREADS)

static ngx_thread_pool_t *
ngx_http_cache_purge_thread_pool(ngx_http_request_t *r) {
    ngx_str_t                  name;
    ngx_thread_pool_t         *tp;
    ngx_http_core_loc_conf_t  *clcf;
    static ngx_str_t           default_name = ngx_string("default");

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    tp = clcf->thread_pool;
    if (tp != NULL) {
        return tp;
    }

    if (clcf->thread_pool_value != NULL) {
        if (ngx_http_complex_value(r, clcf->thread_pool_value, &name) != NGX_OK) {
            return NULL;
        }
    } else {
        name = default_name;
    }

    return ngx_thread_pool_get((ngx_cycle_t *) ngx_cycle, &name);
}

static void
ngx_http_cache_purge_partial_thread(void *data, ngx_log_t *log) {
    ngx_tree_ctx_t                          tree;
    ngx_http_cache_purge_partial_task_ctx_t *ctx;

    ctx = data;

    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));
    tree.init_handler = NULL;
    tree.file_handler = ctx->soft
                        ? ngx_http_purge_file_cache_soft_partial_file
                        : ngx_http_purge_file_cache_delete_partial_file;
    tree.pre_tree_handler = ngx_http_purge_file_cache_noop;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler = ngx_http_purge_file_cache_noop;
    tree.data = &ctx->partial;
    tree.alloc = 0;
    tree.log = log;

    ctx->rc = ngx_walk_tree(&tree, &ctx->partial.cache->path->name) == NGX_OK
              ? NGX_OK : NGX_ERROR;
}

static void
ngx_http_cache_purge_partial_completion(ngx_event_t *ev) {
    ngx_connection_t                        *c;
    ngx_http_request_t                      *r;
    ngx_http_cache_purge_partial_task_ctx_t *ctx;

    ctx = ev->data;
    r = ctx->request;
    c = r->connection;

    ngx_http_set_log_request(c->log, r);

    if (ev->timedout) {
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "partial purge thread operation took too long");
        ev->timedout = 0;
        return;
    }

    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    r->main->blocked--;
    r->aio = 0;

    if (r->done || r->main->terminated) {
        c->write->handler(c->write);
        return;
    }

    if (ctx->rc == NGX_OK) {
        r->write_event_handler = ngx_http_request_empty_handler;
        ngx_http_finalize_request(r, ngx_http_cache_purge_send_response(r));
        ngx_http_run_posted_requests(c);
        return;
    }

    ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
    ngx_http_run_posted_requests(c);
}

#endif /* NGX_CACHE_PURGE_THREADS */

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

    if (ngx_http_cache_purge_enabled(r, cplcf->conf) != NGX_OK) {
        if (cplcf->original_handler) {
            return cplcf->original_handler(r);
        }
        return NGX_DECLINED;
    }

    if (cplcf->handler == NULL) {
        return NGX_HTTP_NOT_FOUND;
    }

    return cplcf->handler(r);
}

ngx_int_t
ngx_http_cache_purge_enabled(ngx_http_request_t *r,
                             ngx_http_cache_purge_conf_t *cpcf) {
    ngx_http_complex_value_t  *cv;
    ngx_str_t                  value;
    ngx_uint_t                 i;

    if (cpcf->enable_values == NULL) {
        return NGX_DECLINED;
    }

    cv = cpcf->enable_values->elts;
    for (i = 0; i < cpcf->enable_values->nelts; i++) {
        if (ngx_http_complex_value(r, &cv[i], &value) != NGX_OK) {
            return NGX_DECLINED;
        }

        if (value.len == 0) {
            continue;
        }

        if (value.len == 1 && value.data[0] == '0') {
            continue;
        }

        return NGX_OK;
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
    u_char       *buf;
    u_char       *buf_keydata;
    const char   *resp_ct;
    size_t        resp_ct_size;
    const char   *resp_body;
    size_t        resp_body_size;

    ngx_http_cache_purge_loc_conf_t   *cplcf;
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    if (r->cache->keys.nelts == 0) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    key = r->cache->keys.elts;

    buf_keydata = ngx_pcalloc(r->pool, key[0].len + 1);
    if (buf_keydata == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    (void) ngx_cpymem(buf_keydata, key[0].data, key[0].len);

    switch (cplcf->resptype) {

    case NGX_RESPONSE_TYPE_JSON:
        resp_ct = ngx_http_cache_purge_content_type_json;
        resp_ct_size = ngx_http_cache_purge_content_type_json_size;
        resp_body = ngx_http_cache_purge_body_templ_json;
        resp_body_size = ngx_http_cache_purge_body_templ_json_size;
        break;

    case NGX_RESPONSE_TYPE_XML:
        resp_ct = ngx_http_cache_purge_content_type_xml;
        resp_ct_size = ngx_http_cache_purge_content_type_xml_size;
        resp_body = ngx_http_cache_purge_body_templ_xml;
        resp_body_size = ngx_http_cache_purge_body_templ_xml_size;
        break;

    case NGX_RESPONSE_TYPE_TEXT:
        resp_ct = ngx_http_cache_purge_content_type_text;
        resp_ct_size = ngx_http_cache_purge_content_type_text_size;
        resp_body = ngx_http_cache_purge_body_templ_text;
        resp_body_size = ngx_http_cache_purge_body_templ_text_size;
        break;

    default:
    case NGX_RESPONSE_TYPE_HTML:
        resp_ct = ngx_http_cache_purge_content_type_html;
        resp_ct_size = ngx_http_cache_purge_content_type_html_size;
        resp_body = ngx_http_cache_purge_body_templ_html;
        resp_body_size = ngx_http_cache_purge_body_templ_html_size;
        break;
    }

    /* resp_body_size = sizeof(template) includes the trailing NUL.
     * Subtract 1 for the NUL and 2 for the "%s" placeholder to get the
     * length of the static parts; add the key length for the full body. */
    len = (resp_body_size - 1) - 2 + key[0].len;

    r->headers_out.content_type.len = resp_ct_size - 1;
    r->headers_out.content_type.data = (u_char *) resp_ct;

    buf = ngx_pcalloc(r->pool, len + 1);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_snprintf(buf, len + 1, resp_body, buf_keydata);

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

    b->last = ngx_cpymem(b->last, buf, len);
    b->last_buf = 1;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
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

static ngx_int_t
ngx_http_cache_purge_init_module(ngx_cycle_t *cycle) {
#if (NGX_LINUX)
    ngx_http_cache_purge_main_conf_t  *pmcf;
    u_char                            *p, dir[NGX_MAX_PATH];
    size_t                             dir_len;
    ngx_file_info_t                    fi;

    pmcf = ngx_http_cycle_get_module_main_conf(cycle,
            ngx_http_cache_purge_module);
    if (pmcf == NULL || pmcf->backend != NGX_HTTP_CACHE_TAG_BACKEND_SQLITE) {
        return NGX_OK;
    }

    /* Validate that the directory component of sqlite_path exists.
     * Runs once in the master process so config reload fails fast. */
    p = pmcf->sqlite_path.data + pmcf->sqlite_path.len;
    while (p > pmcf->sqlite_path.data && p[-1] != '/') {
        p--;
    }

    if (p == pmcf->sqlite_path.data) {
        return NGX_OK;  /* no directory component — relative to cwd, skip */
    }

    dir_len = (size_t)(p - pmcf->sqlite_path.data);
    if (dir_len >= sizeof(dir)) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "cache_tag_index sqlite: path too long");
        return NGX_ERROR;
    }

    ngx_memcpy(dir, pmcf->sqlite_path.data, dir_len);
    dir[dir_len] = '\0';

    if (ngx_file_info(dir, &fi) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno,
                      "cache_tag_index sqlite: directory \"%s\" not found",
                      dir);
        return NGX_ERROR;
    }

    if (!ngx_is_dir(&fi)) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "cache_tag_index sqlite: \"%s\" is not a directory",
                      dir);
        return NGX_ERROR;
    }
#endif

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_purge_init_process(ngx_cycle_t *cycle) {
    ngx_http_cache_purge_main_conf_t *pmcf;

#if !(NGX_LINUX)
    return NGX_OK;
#else
    pmcf = ngx_http_cycle_get_module_main_conf(cycle,
            ngx_http_cache_purge_module);
    if (pmcf == NULL || !ngx_http_cache_tag_store_configured(pmcf)) {
        return NGX_OK;
    }

    return ngx_http_cache_tag_process_init(cycle, pmcf);
#endif
}

void
ngx_http_cache_purge_exit_process(ngx_cycle_t *cycle) {
    (void) cycle;
    ngx_http_cache_tag_process_exit();
}

/*
 * Known limitation: gzip_vary and Vary-based cache variants
 *
 * When gzip_vary (or brotli_vary / zstd_vary) is enabled, nginx stores a
 * separate cache file for each Accept-Encoding variant of the same URL.
 * Each variant has a different on-disk hash derived from the primary key
 * combined with the Vary header value, so a single key-based purge (which
 * uses ngx_http_file_cache_open) can only remove the one variant whose
 * Vary headers match the incoming purge request.
 *
 * Workaround: use cache tags.  Every cached file — including each Vary
 * variant — is independently registered in the tag store at write time, so
 * a tag-based purge finds and removes all variants regardless of encoding.
 * Configure cache_tag / cache_tag_store and tag your responses to take
 * advantage of this.
 */

void
ngx_http_cache_purge_handler(ngx_http_request_t *r) {
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_int_t                           rc;
    ngx_array_t                        *tags;
    ngx_int_t                           mode;

#  if (NGX_HAVE_FILE_AIO)
    if (r->aio) {
        return;
    }
#  endif

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    rc = NGX_OK;
    tags = NULL;
    mode = ngx_http_cache_purge_request_mode(r, cplcf->conf->soft);

    if (ngx_http_cache_tag_location_enabled(cplcf)
            && ngx_http_cache_tag_request_headers(r, &tags) == NGX_OK
            && tags != NULL
            && tags->nelts > 0) {
        rc = NGX_OK;
    } else if (!cplcf->conf->purge_all && !ngx_http_cache_purge_is_partial(r)) {
        if (mode) {
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
ngx_http_cache_purge_request_mode(ngx_http_request_t *r, ngx_flag_t default_soft) {
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *header;
    ngx_uint_t        i;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (cplcf == NULL || cplcf->purge_mode_header.len == 0) {
        return default_soft;
    }

    part = &r->headers_in.headers.part;
    header = part->elts;

    for (i = 0; ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            header = part->elts;
            i = 0;
        }

        if (header[i].key.len != cplcf->purge_mode_header.len) {
            continue;
        }

        if (ngx_strncasecmp(header[i].key.data, cplcf->purge_mode_header.data,
                            cplcf->purge_mode_header.len) != 0) {
            continue;
        }

        if ((header[i].value.len == 1 && header[i].value.data[0] == '1')
                || (header[i].value.len == 4
                    && ngx_strncasecmp(header[i].value.data,
                                       (u_char *) "soft", 4) == 0)
                || (header[i].value.len == 4
                    && ngx_strncasecmp(header[i].value.data,
                                       (u_char *) "true", 4) == 0)) {
            return 1;
        }

        return 0;
    }

    return default_soft;
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
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_http_cache_purge_partial_ctx_t  *ctx;
    ngx_str_t                           *keys;
    ngx_str_t                            key;
    ngx_int_t                            soft;
    ngx_tree_ctx_t                       tree;
#if (NGX_CACHE_PURGE_THREADS)
    ngx_thread_pool_t                   *tp;
    ngx_thread_task_t                   *task;
    ngx_http_cache_purge_partial_task_ctx_t *tctx;
#endif

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "purge_partial http in %s",
                  cache->path->name.data);

    if (r->cache->keys.nelts == 0) {
        return NGX_ERROR;
    }

    /* Only check the first key, and discard '*' at the end */
    keys = r->cache->keys.elts;
    key = keys[0];
    if (key.len == 0) {
        return NGX_ERROR;
    }
    key.len--;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    soft = ngx_http_cache_purge_request_mode(r, cplcf->conf->soft);

#if (NGX_CACHE_PURGE_THREADS)
    /* If a thread pool is available, offload the blocking directory walk.
     * Falls through to the synchronous path when no pool is configured. */
    tp = ngx_http_cache_purge_thread_pool(r);
    if (tp != NULL) {
        task = ngx_thread_task_alloc(r->pool,
                                     sizeof(ngx_http_cache_purge_partial_task_ctx_t));
        if (task == NULL) {
            return NGX_ERROR;
        }

        tctx = task->ctx;
        ngx_memzero(tctx, sizeof(ngx_http_cache_purge_partial_task_ctx_t));
        tctx->request = r;
        tctx->partial.cache = cache;
        tctx->partial.key_len = key.len;
        tctx->soft = soft;

        if (key.len > 0) {
            tctx->partial.key_partial = key.data;
            tctx->partial.key_in_file = ngx_pnalloc(r->pool,
                                                    sizeof(u_char) * key.len);
            if (tctx->partial.key_in_file == NULL) {
                return NGX_ERROR;
            }
        }

        tctx->rc = NGX_ERROR;
        task->handler = ngx_http_cache_purge_partial_thread;
        task->event.data = tctx;
        task->event.handler = ngx_http_cache_purge_partial_completion;
        task->event.log = r->connection->log;
        task->event.cancelable = 1;

        if (ngx_thread_task_post(tp, task) != NGX_OK) {
            return NGX_ERROR;
        }

        ngx_add_timer(&task->event, 60000);
        r->main->blocked++;
        r->aio = 1;

        return NGX_DONE;
    }
#endif

    /* Synchronous fallback: no thread pool configured or threads not built. */
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

    /* Walk the tree and remove all the files matching key_partial */
    tree.init_handler = NULL;
    tree.file_handler = soft
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
    ngx_http_compile_complex_value_t   ccv;
    ngx_http_complex_value_t          *cv;
    ngx_str_t                         *value;
    ngx_uint_t                         position, last_cond;

    value = cf->args->elts;

    if (cf->args->nelts < 2) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid syntax, expected "
                           "\"string ... [soft] [purge_all]\"");
        return NGX_CONF_ERROR;
    }

    cpcf->enable = 1;
    cpcf->configured = 1;
    position = cf->args->nelts;

    for (;;) {
        if (position > 2 && !cpcf->purge_all
                && ngx_strcmp(value[position - 1].data, "purge_all") == 0) {
            cpcf->purge_all = 1;
            position--;
            continue;
        }

        if (position > 2 && !cpcf->soft
                && ngx_strcmp(value[position - 1].data, "soft") == 0) {
            cpcf->soft = 1;
            position--;
            continue;
        }

        break;
    }

    if (position < 2) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid syntax, expected at least one purge condition before "
                           "\"soft\" or \"purge_all\"");
        return NGX_CONF_ERROR;
    }

    last_cond = position - 1;
    cpcf->enable_values = ngx_array_create(cf->pool, last_cond, sizeof(ngx_http_complex_value_t));
    if (cpcf->enable_values == NULL) {
        return NGX_CONF_ERROR;
    }

    for (position = 1; position <= last_cond; position++) {
        cv = ngx_array_push(cpcf->enable_values);
        if (cv == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
        ccv.cf = cf;
        ccv.value = &value[position];
        ccv.complex_value = cv;

        if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}

void
ngx_http_cache_purge_merge_conf(ngx_http_cache_purge_conf_t *conf,
                                ngx_http_cache_purge_conf_t *prev) {
    if (conf->enable == NGX_CONF_UNSET) {
        if (prev->enable == 1) {
            conf->enable = prev->enable;
            conf->configured = 0;
            conf->enable_values = prev->enable_values;
            conf->soft = prev->soft;
            conf->purge_all = prev->purge_all;
        } else {
            conf->enable = 0;
            conf->configured = 0;
        }
    }
}

static ngx_int_t
ngx_http_cache_purge_complex_value_set(ngx_http_complex_value_t *cv) {
    return cv->lengths != NULL || cv->value.len != 0;
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

    conf->queue_shm_size = NGX_CONF_UNSET_SIZE;

    return conf;
}

char *
ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf) {
    ngx_http_cache_purge_main_conf_t  *pmcf = conf;

    ngx_conf_init_size_value(pmcf->queue_shm_size, NGX_HTTP_CACHE_TAG_QUEUE_SIZE);

#if (NGX_LINUX)
    if (ngx_http_cache_tag_store_configured(pmcf)
            && ngx_http_cache_tag_queue_init_conf(cf, pmcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }
#endif

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
     *     conf->*.enable_values = NULL
     *     conf->handler = NULL
     *     conf->original_handler = NULL
     */

# if (NGX_HTTP_FASTCGI)
    conf->fastcgi.enable = NGX_CONF_UNSET;
    conf->fastcgi.configured = NGX_CONF_UNSET;
# endif /* NGX_HTTP_FASTCGI */
# if (NGX_HTTP_PROXY)
    conf->proxy.enable = NGX_CONF_UNSET;
    conf->proxy.configured = NGX_CONF_UNSET;
# endif /* NGX_HTTP_PROXY */
# if (NGX_HTTP_SCGI)
    conf->scgi.enable = NGX_CONF_UNSET;
    conf->scgi.configured = NGX_CONF_UNSET;
# endif /* NGX_HTTP_SCGI */
# if (NGX_HTTP_UWSGI)
    conf->uwsgi.enable = NGX_CONF_UNSET;
    conf->uwsgi.configured = NGX_CONF_UNSET;
# endif /* NGX_HTTP_UWSGI */

    conf->resptype = NGX_CONF_UNSET_UINT;
    conf->cache_tag_watch = NGX_CONF_UNSET;
    conf->purge_mode_header.len = 0;
    conf->purge_mode_header.data = NULL;

    conf->conf = NGX_CONF_UNSET_PTR;

    return conf;
}

#if (NGX_HTTP_FASTCGI)
static char *
ngx_http_fastcgi_cache_purge_attach(ngx_conf_t *cf,
                                    ngx_http_cache_purge_loc_conf_t *conf,
                                    ngx_http_core_loc_conf_t *clcf,
                                    ngx_http_fastcgi_loc_conf_t *flcf) {
    ngx_flag_t  has_cache_key;
    ngx_flag_t  has_cache;
    ngx_flag_t  has_pass;

    has_cache_key = ngx_http_cache_purge_complex_value_set(&flcf->cache_key);
    has_pass = flcf->upstream.upstream || flcf->fastcgi_lengths;
#  if (nginx_version >= 1007009)
    has_cache = flcf->upstream.cache > 0
                || flcf->upstream.cache_zone != NULL
                || flcf->upstream.cache_value != NULL;
#  else
    has_cache = flcf->upstream.cache != NGX_CONF_UNSET_PTR
                && flcf->upstream.cache != NULL;
#  endif

    if (!has_pass && !has_cache && !has_cache_key) {
        if (!conf->fastcgi.configured || clcf->name.len == 0) {
            return NGX_CONF_OK;
        }
    }

    if (!conf->fastcgi.configured && !has_pass) {
        return NGX_CONF_OK;
    }

    if (has_pass) {
        conf->conf = &conf->fastcgi;
        conf->handler = (has_cache && has_cache_key)
                        ? ngx_http_fastcgi_cache_purge_handler : NULL;
        conf->original_handler = clcf->handler;
        clcf->handler = ngx_http_cache_purge_access_handler;

        if (conf->cache_tag_watch && conf->handler != NULL
                && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                        flcf->upstream.cache_zone ? flcf->upstream.cache_zone->data : NULL
#  else
                        flcf->upstream.cache ? flcf->upstream.cache->data : NULL
#  endif
                        , conf->cache_tag_headers) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    if (!has_cache_key) {
        return "\"fastcgi_cache_purge\" requires \"fastcgi_cache_key\" on this location";
    }

    if (!has_cache) {
        return "\"fastcgi_cache_purge\" requires \"fastcgi_cache\" on this location";
    }

    conf->conf = &conf->fastcgi;
    conf->handler = ngx_http_fastcgi_cache_purge_handler;
    conf->original_handler = clcf->handler;
    clcf->handler = ngx_http_cache_purge_access_handler;

    if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
            flcf->upstream.cache_zone ? flcf->upstream.cache_zone->data : NULL
#  else
            flcf->upstream.cache ? flcf->upstream.cache->data : NULL
#  endif
            , conf->cache_tag_headers) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
#endif

#if (NGX_HTTP_PROXY)
static char *
ngx_http_proxy_cache_purge_attach(ngx_conf_t *cf,
                                  ngx_http_cache_purge_loc_conf_t *conf,
                                  ngx_http_core_loc_conf_t *clcf,
                                  ngx_http_proxy_loc_conf_t *plcf) {
    ngx_flag_t  has_cache_key;
    ngx_flag_t  has_cache;
    ngx_flag_t  has_pass;

    has_cache_key = ngx_http_cache_purge_complex_value_set(&plcf->cache_key);
    has_pass = plcf->upstream.upstream || plcf->proxy_lengths;
#  if (nginx_version >= 1007009)
    has_cache = plcf->upstream.cache > 0
                || plcf->upstream.cache_zone != NULL
                || plcf->upstream.cache_value != NULL;
#  else
    has_cache = plcf->upstream.cache != NGX_CONF_UNSET_PTR
                && plcf->upstream.cache != NULL;
#  endif

    if (!has_pass && !has_cache && !has_cache_key) {
        if (!conf->proxy.configured || clcf->name.len == 0) {
            return NGX_CONF_OK;
        }
    }

    if (!conf->proxy.configured && !has_pass) {
        return NGX_CONF_OK;
    }

    if (has_pass) {
        conf->conf = &conf->proxy;
        conf->handler = (has_cache && has_cache_key)
                        ? ngx_http_proxy_cache_purge_handler : NULL;
        conf->original_handler = clcf->handler;
        clcf->handler = ngx_http_cache_purge_access_handler;

        if (conf->cache_tag_watch && conf->handler != NULL
                && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                        plcf->upstream.cache_zone ? plcf->upstream.cache_zone->data : NULL
#  else
                        plcf->upstream.cache ? plcf->upstream.cache->data : NULL
#  endif
                        , conf->cache_tag_headers) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    if (!has_cache_key) {
        return "\"proxy_cache_purge\" requires \"proxy_cache_key\" on this location";
    }

    if (!has_cache) {
        return "\"proxy_cache_purge\" requires \"proxy_cache\" on this location";
    }

    conf->conf = &conf->proxy;
    conf->handler = ngx_http_proxy_cache_purge_handler;
    conf->original_handler = clcf->handler;
    clcf->handler = ngx_http_cache_purge_access_handler;

    if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
            plcf->upstream.cache_zone ? plcf->upstream.cache_zone->data : NULL
#  else
            plcf->upstream.cache ? plcf->upstream.cache->data : NULL
#  endif
            , conf->cache_tag_headers) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
#endif

#if (NGX_HTTP_SCGI)
static char *
ngx_http_scgi_cache_purge_attach(ngx_conf_t *cf,
                                 ngx_http_cache_purge_loc_conf_t *conf,
                                 ngx_http_core_loc_conf_t *clcf,
                                 ngx_http_scgi_loc_conf_t *slcf) {
    ngx_flag_t  has_cache_key;
    ngx_flag_t  has_cache;
    ngx_flag_t  has_pass;

    has_cache_key = ngx_http_cache_purge_complex_value_set(&slcf->cache_key);
    has_pass = slcf->upstream.upstream || slcf->scgi_lengths;
#  if (nginx_version >= 1007009)
    has_cache = slcf->upstream.cache > 0
                || slcf->upstream.cache_zone != NULL
                || slcf->upstream.cache_value != NULL;
#  else
    has_cache = slcf->upstream.cache != NGX_CONF_UNSET_PTR
                && slcf->upstream.cache != NULL;
#  endif

    if (!has_pass && !has_cache && !has_cache_key) {
        if (!conf->scgi.configured || clcf->name.len == 0) {
            return NGX_CONF_OK;
        }
    }

    if (!conf->scgi.configured && !has_pass) {
        return NGX_CONF_OK;
    }

    if (has_pass) {
        conf->conf = &conf->scgi;
        conf->handler = (has_cache && has_cache_key)
                        ? ngx_http_scgi_cache_purge_handler : NULL;
        conf->original_handler = clcf->handler;
        clcf->handler = ngx_http_cache_purge_access_handler;

        if (conf->cache_tag_watch && conf->handler != NULL
                && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                        slcf->upstream.cache_zone ? slcf->upstream.cache_zone->data : NULL
#  else
                        slcf->upstream.cache ? slcf->upstream.cache->data : NULL
#  endif
                        , conf->cache_tag_headers) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    if (!has_cache_key) {
        return "\"scgi_cache_purge\" requires \"scgi_cache_key\" on this location";
    }

    if (!has_cache) {
        return "\"scgi_cache_purge\" requires \"scgi_cache\" on this location";
    }

    conf->conf = &conf->scgi;
    conf->handler = ngx_http_scgi_cache_purge_handler;
    conf->original_handler = clcf->handler;
    clcf->handler = ngx_http_cache_purge_access_handler;

    if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
            slcf->upstream.cache_zone ? slcf->upstream.cache_zone->data : NULL
#  else
            slcf->upstream.cache ? slcf->upstream.cache->data : NULL
#  endif
            , conf->cache_tag_headers) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
#endif

#if (NGX_HTTP_UWSGI)
static char *
ngx_http_uwsgi_cache_purge_attach(ngx_conf_t *cf,
                                  ngx_http_cache_purge_loc_conf_t *conf,
                                  ngx_http_core_loc_conf_t *clcf,
                                  ngx_http_uwsgi_loc_conf_t *ulcf) {
    ngx_flag_t  has_cache_key;
    ngx_flag_t  has_cache;
    ngx_flag_t  has_pass;

    has_cache_key = ngx_http_cache_purge_complex_value_set(&ulcf->cache_key);
    has_pass = ulcf->upstream.upstream || ulcf->uwsgi_lengths;
#  if (nginx_version >= 1007009)
    has_cache = ulcf->upstream.cache > 0
                || ulcf->upstream.cache_zone != NULL
                || ulcf->upstream.cache_value != NULL;
#  else
    has_cache = ulcf->upstream.cache != NGX_CONF_UNSET_PTR
                && ulcf->upstream.cache != NULL;
#  endif

    if (!has_pass && !has_cache && !has_cache_key) {
        if (!conf->uwsgi.configured || clcf->name.len == 0) {
            return NGX_CONF_OK;
        }
    }

    if (!conf->uwsgi.configured && !has_pass) {
        return NGX_CONF_OK;
    }

    if (has_pass) {
        conf->conf = &conf->uwsgi;
        conf->handler = (has_cache && has_cache_key)
                        ? ngx_http_uwsgi_cache_purge_handler : NULL;
        conf->original_handler = clcf->handler;
        clcf->handler = ngx_http_cache_purge_access_handler;

        if (conf->cache_tag_watch && conf->handler != NULL
                && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
                        ulcf->upstream.cache_zone ? ulcf->upstream.cache_zone->data : NULL
#  else
                        ulcf->upstream.cache ? ulcf->upstream.cache->data : NULL
#  endif
                        , conf->cache_tag_headers) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    if (!has_cache_key) {
        return "\"uwsgi_cache_purge\" requires \"uwsgi_cache_key\" on this location";
    }

    if (!has_cache) {
        return "\"uwsgi_cache_purge\" requires \"uwsgi_cache\" on this location";
    }

    conf->conf = &conf->uwsgi;
    conf->handler = ngx_http_uwsgi_cache_purge_handler;
    conf->original_handler = clcf->handler;
    clcf->handler = ngx_http_cache_purge_access_handler;

    if (conf->cache_tag_watch && ngx_http_cache_tag_register_cache(cf,
#  if (nginx_version >= 1007009)
            ulcf->upstream.cache_zone ? ulcf->upstream.cache_zone->data : NULL
#  else
            ulcf->upstream.cache ? ulcf->upstream.cache->data : NULL
#  endif
            , conf->cache_tag_headers) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
#endif

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

    ngx_conf_merge_uint_value(conf->resptype, prev->resptype, NGX_RESPONSE_TYPE_HTML);
    ngx_conf_merge_value(conf->cache_tag_watch, prev->cache_tag_watch, 0);
    ngx_conf_merge_str_value(conf->purge_mode_header, prev->purge_mode_header, "");

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

    if (conf->fastcgi.enable) {
        flcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_fastcgi_module);
        return ngx_http_fastcgi_cache_purge_attach(cf, conf, clcf, flcf);
    }
# endif /* NGX_HTTP_FASTCGI */

# if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_merge_conf(&conf->proxy, &prev->proxy);

    if (conf->proxy.enable) {
        plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_proxy_module);
        return ngx_http_proxy_cache_purge_attach(cf, conf, clcf, plcf);
    }
# endif /* NGX_HTTP_PROXY */

# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_merge_conf(&conf->scgi, &prev->scgi);

    if (conf->scgi.enable) {
        slcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_scgi_module);
        return ngx_http_scgi_cache_purge_attach(cf, conf, clcf, slcf);
    }
# endif /* NGX_HTTP_SCGI */

# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_merge_conf(&conf->uwsgi, &prev->uwsgi);

    if (conf->uwsgi.enable) {
        ulcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_uwsgi_module);
        return ngx_http_uwsgi_cache_purge_attach(cf, conf, clcf, ulcf);
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
