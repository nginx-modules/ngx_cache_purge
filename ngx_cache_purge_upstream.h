#ifndef _NGX_CACHE_PURGE_UPSTREAM_H_INCLUDED_
#define _NGX_CACHE_PURGE_UPSTREAM_H_INCLUDED_

/* Use exported core layouts where available; keep old-ABI adapters together. */
#include "ngx_cache_purge.h"

#if (NGX_HTTP_CACHE)
# if (NGX_HTTP_FASTCGI)
extern ngx_module_t  ngx_http_fastcgi_module;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_fastcgi_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_uint_t    number;
    ngx_hash_t    hash;
} ngx_http_fastcgi_params_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t  upstream;
    ngx_str_t                 index;

#  if (nginx_version >= 1007008)
    ngx_http_fastcgi_params_t  params;
    ngx_http_fastcgi_params_t  params_cache;
#  else
    ngx_array_t  *flushes;
    ngx_array_t  *params_len;
    ngx_array_t  *params;
#  endif

    ngx_array_t  *params_source;
    ngx_array_t  *catch_stderr;
    ngx_array_t  *fastcgi_lengths;
    ngx_array_t  *fastcgi_values;

#  if (nginx_version >= 8040) && (nginx_version < 1007008)
    ngx_hash_t  headers_hash;
    ngx_uint_t  header_params;
#  endif

#  if (nginx_version >= 1001004)
    ngx_flag_t  keep_conn;
#  endif

    ngx_http_complex_value_t  cache_key;

#  if (NGX_PCRE)
    ngx_regex_t  *split_regex;
    ngx_str_t     split_name;
#  endif
} ngx_http_fastcgi_loc_conf_t;

# endif

# if (NGX_HTTP_PROXY)
#  if (nginx_version >= 1029004)
/* Prefer the exported nginx header to a private definition. */
#   include <ngx_http_proxy_module.h>
#  else
extern ngx_module_t  ngx_http_proxy_module;

typedef struct {
    ngx_str_t  key_start;
    ngx_str_t  schema;
    ngx_str_t  host_header;
    ngx_str_t  port;
    ngx_str_t  uri;
} ngx_http_proxy_vars_t;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_proxy_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_hash_t    hash;
} ngx_http_proxy_headers_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t  upstream;

#  if (nginx_version >= 1007008)
    ngx_array_t              *body_flushes;
    ngx_array_t              *body_lengths;
    ngx_array_t              *body_values;
    ngx_str_t                 body_source;
    ngx_http_proxy_headers_t  headers;
    ngx_http_proxy_headers_t  headers_cache;
#  else
    ngx_array_t  *flushes;
    ngx_array_t  *body_set_len;
    ngx_array_t  *body_set;
    ngx_array_t  *headers_set_len;
    ngx_array_t  *headers_set;
    ngx_hash_t    headers_set_hash;
#  endif

    ngx_array_t  *headers_source;
#  if (nginx_version < 8040)
    ngx_array_t  *headers_names;
#  endif

    ngx_array_t  *proxy_lengths;
    ngx_array_t  *proxy_values;
    ngx_array_t  *redirects;

#  if (nginx_version >= 1001015)
    ngx_array_t  *cookie_domains;
    ngx_array_t  *cookie_paths;
#  endif
#  if (nginx_version >= 1019003)
    ngx_array_t  *cookie_flags;
#  endif
#  if (nginx_version < 1007008)
    ngx_str_t  body_source;
#  endif

#  if (nginx_version >= 1011006)
    ngx_http_complex_value_t  *method;
#  else
    ngx_str_t  method;
#  endif
    ngx_str_t  location;
    ngx_str_t  url;

    ngx_http_complex_value_t  cache_key;
    ngx_http_proxy_vars_t     vars;
    ngx_flag_t                redirect;

#  if (nginx_version >= 1001004)
    ngx_uint_t  http_version;
#  endif

    ngx_uint_t  headers_hash_max_size;
    ngx_uint_t  headers_hash_bucket_size;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005006)
    ngx_uint_t  ssl;
    ngx_uint_t  ssl_protocols;
    ngx_str_t   ssl_ciphers;
#    endif
#    if (nginx_version >= 1007000)
    ngx_uint_t  ssl_verify_depth;
    ngx_str_t   ssl_trusted_certificate;
    ngx_str_t   ssl_crl;
#    endif
#    if (nginx_version >= 1007008) && (nginx_version < 1021000)
    ngx_str_t     ssl_certificate;
    ngx_str_t     ssl_certificate_key;
    ngx_array_t  *ssl_passwords;
#    endif
#    if (nginx_version >= 1019004)
    ngx_array_t  *ssl_conf_commands;
#    endif
#  endif
} ngx_http_proxy_loc_conf_t;
#  endif /* nginx_version >= 1029004 */

# endif

# if (NGX_HTTP_SCGI)
extern ngx_module_t  ngx_http_scgi_module;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_scgi_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_uint_t    number;
    ngx_hash_t    hash;
} ngx_http_scgi_params_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t  upstream;

#  if (nginx_version >= 1007008)
    ngx_http_scgi_params_t  params;
    ngx_http_scgi_params_t  params_cache;
    ngx_array_t            *params_source;
#  else
    ngx_array_t  *flushes;
    ngx_array_t  *params_len;
    ngx_array_t  *params;
    ngx_array_t  *params_source;
    ngx_hash_t    headers_hash;
    ngx_uint_t    header_params;
#  endif

    ngx_array_t              *scgi_lengths;
    ngx_array_t              *scgi_values;
    ngx_http_complex_value_t  cache_key;
} ngx_http_scgi_loc_conf_t;

# endif

# if (NGX_HTTP_UWSGI)
extern ngx_module_t  ngx_http_uwsgi_module;

#  if (nginx_version >= 1007009)
typedef struct {
    ngx_array_t  caches;
} ngx_http_uwsgi_main_conf_t;
#  endif

#  if (nginx_version >= 1007008)
typedef struct {
    ngx_array_t  *flushes;
    ngx_array_t  *lengths;
    ngx_array_t  *values;
    ngx_uint_t    number;
    ngx_hash_t    hash;
} ngx_http_uwsgi_params_t;
#  endif

typedef struct {
    ngx_http_upstream_conf_t  upstream;

#  if (nginx_version >= 1007008)
    ngx_http_uwsgi_params_t  params;
    ngx_http_uwsgi_params_t  params_cache;
    ngx_array_t             *params_source;
#  else
    ngx_array_t  *flushes;
    ngx_array_t  *params_len;
    ngx_array_t  *params;
    ngx_array_t  *params_source;
    ngx_hash_t    headers_hash;
    ngx_uint_t    header_params;
#  endif

    ngx_array_t              *uwsgi_lengths;
    ngx_array_t              *uwsgi_values;
    ngx_http_complex_value_t  cache_key;
    ngx_str_t                 uwsgi_string;
    ngx_uint_t                modifier1;
    ngx_uint_t                modifier2;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005008)
    ngx_uint_t  ssl;
    ngx_uint_t  ssl_protocols;
    ngx_str_t   ssl_ciphers;
#    endif
#    if (nginx_version >= 1007000)
    ngx_uint_t  ssl_verify_depth;
    ngx_str_t   ssl_trusted_certificate;
    ngx_str_t   ssl_crl;
#    endif
#    if (nginx_version >= 1007008) && (nginx_version < 1021000)
    ngx_str_t     ssl_certificate;
    ngx_str_t     ssl_certificate_key;
    ngx_array_t  *ssl_passwords;
#    endif
#    if (nginx_version >= 1019004)
    ngx_array_t  *ssl_conf_commands;
#    endif
#  endif
} ngx_http_uwsgi_loc_conf_t;

# endif

#endif
#endif
