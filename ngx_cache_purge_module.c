/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * Copyright (C) 2016-2026, Denys Denysov
 * All rights reserved.
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


#include "ngx_cache_purge.h"
#include "ngx_cache_purge_upstream.h"

#if (NGX_HTTP_CACHE)

/* -- module commands ---------------------------------------------------- */


static ngx_int_t
ngx_http_cache_purge_key_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t
ngx_http_cache_purge_variables(ngx_conf_t *cf);
static char *
ngx_http_cache_purge_namespace_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *
ngx_http_cache_purge_key_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *
ngx_http_cache_purge_legacy(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static char *
ngx_http_cache_purge_flag(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *
ngx_http_cache_purge_throttle(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *
ngx_http_cache_purge_thread_pool(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *
ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf);
static char *
ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t
ngx_http_cache_purge_postconfiguration(ngx_conf_t *cf);
static char *
ngx_http_cache_purge_queue_status_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
#if (NGX_HTTP_FASTCGI)
char *
ngx_http_fastcgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
#endif
#if (NGX_HTTP_SCGI)
char *
ngx_http_scgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
#endif
#if (NGX_HTTP_UWSGI)
char *
ngx_http_uwsgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
#endif
char *
ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
char *
ngx_http_cache_purge_conf(ngx_conf_t *cf, ngx_http_cache_purge_conf_t *cpcf);
#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
static void
ngx_http_cache_purge_merge_conf(ngx_http_cache_purge_conf_t *conf,
    ngx_http_cache_purge_conf_t *prev);
#endif
void *
ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf);
char *
ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_command_t  ngx_http_cache_purge_module_commands[] = {

    { ngx_string("cache_purge_namespace"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE3,
      ngx_http_cache_purge_namespace_conf,
      NGX_HTTP_MAIN_CONF_OFFSET, 0, NULL },

    { ngx_string("cache_purge_key"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_cache_purge_key_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

# if (NGX_HTTP_FASTCGI)
    { ngx_string("fastcgi_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_fastcgi_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

# if (NGX_HTTP_PROXY)
    { ngx_string("proxy_cache_refresh"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_cache_refresh_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    { ngx_string("cache_purge_refresh_profile"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_cache_refresh_profile,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    { ngx_string("cache_purge_refresh_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_loc_conf_t, refresh_timeout), NULL },

    { ngx_string("proxy_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_proxy_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

# if (NGX_HTTP_SCGI)
    { ngx_string("scgi_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_scgi_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

# if (NGX_HTTP_UWSGI)
    { ngx_string("uwsgi_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_uwsgi_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

    { ngx_string("cache_purge_response_type"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_response_type_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    { ngx_string("cache_purge_thread_pool"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_thread_pool,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    { ngx_string("cache_purge_background_queue"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG,
      ngx_http_cache_purge_flag,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, background_purge), NULL },

    { ngx_string("cache_purge_queue_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, queue_size), NULL },

    { ngx_string("cache_purge_batch_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, batch_size), NULL },

    /* The initial delay and bare integer settings are milliseconds. */
    { ngx_string("cache_purge_throttle_ms"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_throttle,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, throttle_ms), NULL },

    { ngx_string("cache_purge_legacy_status"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG,
      ngx_http_cache_purge_legacy,
      NGX_HTTP_MAIN_CONF_OFFSET, 0, NULL },

    { ngx_string("cache_purge_vary_aware"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_FLAG,
      ngx_http_cache_purge_flag,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, vary_aware), NULL },

    { ngx_string("cache_purge_queue_status"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_queue_status_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    ngx_null_command
};


/* -- module context & descriptor ---------------------------------------- */

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    ngx_http_cache_purge_variables,          /* preconfiguration  */
    ngx_http_cache_purge_postconfiguration, /* postconfiguration */
    ngx_http_cache_purge_create_main_conf,  /* create main conf  */
    ngx_http_cache_purge_init_main_conf,    /* init main conf    */
    NULL,                                   /* create srv conf   */
    NULL,                                   /* merge srv conf    */
    ngx_http_cache_purge_create_loc_conf,   /* create loc conf   */
    ngx_http_cache_purge_merge_loc_conf     /* merge loc conf    */
};

/*
 * A planned shutdown is the only point where the namespace rules can be
 * committed without adding file I/O to the invalidation path.  When the
 * commit fails, every running marker is kept so the next start rotates the
 * epoch rather than trusting a partially written snapshot.
 */
static void
ngx_http_cache_purge_exit_master(ngx_cycle_t *cycle)
{
    ngx_http_cache_purge_main_conf_t   *cmcf;
    ngx_http_cache_purge_namespace_t  **namespaces;
    ngx_uint_t                          i;
    ngx_flag_t                          failed;

    cmcf = ngx_http_cycle_get_module_main_conf(cycle,
                                               ngx_http_cache_purge_module);
    if (cmcf == NULL || cmcf->namespaces.nelts == 0) {
        return;
    }

    namespaces = cmcf->namespaces.elts;
    failed = 0;

    for (i = 0; i < cmcf->namespaces.nelts; i++) {
        if (ngx_http_cache_purge_generation_persist(namespaces[i])
            != NGX_OK)
        {
            failed = 1;
        }
    }

    if (failed) {
        return;
    }

    for (i = 0; i < cmcf->namespaces.nelts; i++) {
        ngx_http_cache_purge_generation_clear_running(namespaces[i], ngx_pid);
    }
}


ngx_module_t  ngx_http_cache_purge_module = {
    NGX_MODULE_V1,
    &ngx_http_cache_purge_module_ctx,
    ngx_http_cache_purge_module_commands,
    NGX_HTTP_MODULE,
    NULL,                                   /* init master  */
    NULL,                                   /* init module  */
    ngx_http_cache_purge_init_worker,       /* init process */
    NULL,                                   /* init thread  */
    NULL,                                   /* exit thread  */
    ngx_http_cache_purge_exit_worker,       /* exit process */
    ngx_http_cache_purge_exit_master,       /* exit master  */
    NGX_MODULE_V1_PADDING
};

/* Each worker owns one fixed key array and one active walk. */
/* -- main configuration ------------------------------------------------- */

static ngx_int_t
ngx_http_cache_purge_key_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_cache_purge_loc_conf_t *conf;
    ngx_str_t                       logical, encoded;

    (void) data;
    conf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (conf->namespace == NULL) {
        v->not_found = 1;
        return NGX_OK;
    }
    if (ngx_http_cache_purge_generation_key(r, conf->namespace,
                        &conf->logical_key, &logical, &encoded) != NGX_OK)
    {
        return NGX_ERROR;
    }
    v->data = encoded.data;
    v->len = encoded.len;
    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;
    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_variables(ngx_conf_t *cf)
{
    ngx_str_t             name = ngx_string("cache_purge_key");
    ngx_http_variable_t  *v;
# if (NGX_HTTP_PROXY)
    ngx_uint_t i;
    ngx_str_t names[] = {
        ngx_string("cache_purge_refresh_uri"),
        ngx_string("cache_purge_refresh_etag"),
        ngx_string("cache_purge_refresh_last_modified")
    };
# endif

    v = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_NOCACHEABLE);
    if (v == NULL) {
        return NGX_ERROR;
    }
    v->get_handler = ngx_http_cache_purge_key_variable;
# if (NGX_HTTP_PROXY)
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        v = ngx_http_add_variable(cf, &names[i], NGX_HTTP_VAR_NOCACHEABLE);
        if (v == NULL) {
            return NGX_ERROR;
        }
        v->get_handler = ngx_http_cache_refresh_variable;
        v->data = i;
    }
# endif
    return NGX_OK;
}


static char *
ngx_http_cache_purge_namespace_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf = conf;
    ngx_http_cache_purge_namespace_t **ns, *created;
    ngx_str_t                       *value;
    ssize_t                          size;

    (void) cmd;
    value = cf->args->elts;
    size = ngx_parse_size(&value[2]);
    if (size == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "ngx_cache_purge: invalid namespace size \"%V\" "
                          "for namespace \"%V\"", &value[2], &value[1]);
        return NGX_CONF_ERROR;
    }
    created = ngx_http_cache_purge_namespace_create(cf, &value[1],
                                                    (size_t) size, &value[3]);
    if (created == NULL) {
        return NGX_CONF_ERROR;
    }
    ns = ngx_array_push(&cmcf->namespaces);
    if (ns == NULL) {
        return NGX_CONF_ERROR;
    }
    *ns = created;
    return NGX_CONF_OK;
}


static char *
ngx_http_cache_purge_key_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cache_purge_loc_conf_t *cplcf = conf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_http_cache_purge_namespace_t **ns;
    ngx_http_compile_complex_value_t ccv;
    ngx_str_t                       *value;
    ngx_uint_t                       i;

    (void) cmd;
    if (cplcf->namespace != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);
    value = cf->args->elts;
    ns = cmcf->namespaces.elts;
    for (i = 0; i < cmcf->namespaces.nelts; i++) {
        if (ns[i]->name.len == value[1].len
            && ngx_memcmp(ns[i]->name.data, value[1].data, value[1].len) == 0)
        {
            cplcf->namespace = ns[i];
            break;
        }
    }
    if (cplcf->namespace == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "ngx_cache_purge: unknown namespace \"%V\" in "
                          "\"%V\"; declare it before use",
                          &value[1], &cmd->name);
        return NGX_CONF_ERROR;
    }
    ngx_memzero(&ccv, sizeof(ccv));
    ccv.cf = cf;
    ccv.value = &value[2];
    ccv.complex_value = &cplcf->logical_key;
    return ngx_http_compile_complex_value(&ccv) == NGX_OK
           ? NGX_CONF_OK : NGX_CONF_ERROR;
}


static char *
ngx_http_cache_purge_legacy(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cache_purge_main_conf_t  *cmcf = conf;
    ngx_str_t                        *value = cf->args->elts;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") != 0
        && ngx_strcasecmp(value[1].data, (u_char *) "off") != 0)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "ngx_cache_purge: \"%V\" directive has invalid value \"%V\", "
            "expected \"on\" or \"off\"", &cmd->name, &value[1]);
        return NGX_CONF_ERROR;
    }
    if (!cmcf->legacy_status_seen) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
            "ngx_cache_purge: cache_purge_legacy_status is obsolete and ignored; "
            "v4 uses HTTP status and Cache-Purge-Code; remove this directive");
        cmcf->legacy_status_seen = 1;
    }
    return NGX_CONF_OK;
}


static char *
ngx_http_cache_purge_flag(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t   *value;
    ngx_flag_t  *flag;

    value = cf->args->elts;
    flag = (ngx_flag_t *) ((u_char *) conf + cmd->offset);

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        *flag = 1;

    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        *flag = 0;

    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "has invalid value \"%V\", expected \"on\" or "
                           "\"off\"", &cmd->name, &value[1]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_cache_purge_throttle(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t   *value;
    ngx_msec_t  *delay;
    ngx_int_t    n;

    value = cf->args->elts;
    delay = (ngx_msec_t *) ((u_char *) conf + cmd->offset);

    if (*delay != NGX_CONF_UNSET_MSEC) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }

    n = ngx_atoi(value[1].data, value[1].len);
    if (n == NGX_ERROR) {
        return ngx_conf_set_msec_slot(cf, cmd, conf);
    }

    *delay = (ngx_msec_t) n;
    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "ngx_cache_purge: cache_purge_throttle_ms value \"%V\" "
                       "has no time "
                       "suffix, interpreting it as milliseconds; "
                       "use an explicit ms suffix", &value[1]);

    return NGX_CONF_OK;
}


static char *
ngx_http_cache_purge_thread_pool(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cache_purge_loc_conf_t  *cplcf = conf;
    ngx_str_t                       *value;

    (void) cmd;
    if (cplcf->thread_pool_name.data != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }
    value = cf->args->elts;
    if (value[1].len == 0 || value[1].len > NGX_CACHE_PURGE_KEY_MAX_LEN) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "requires a pool name of 1 to %d bytes; got %uz",
                           &cmd->name, NGX_CACHE_PURGE_KEY_MAX_LEN, value[1].len);
        return NGX_CONF_ERROR;
    }
    cplcf->thread_pool_name = value[1];
# if (NGX_THREADS && !NGX_WIN32)
    if (ngx_thread_pool_add(cf, &cplcf->thread_pool_name) == NULL) {
        return NGX_CONF_ERROR;
    }
# endif
    return NGX_CONF_OK;
}


static void *
ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;

    cmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_main_conf_t));
    if (cmcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&cmcf->queue_status_locations, cf->pool, 1,
                       sizeof(ngx_http_cache_purge_loc_conf_t *)) != NGX_OK)
    {
        return NULL;
    }
    if (ngx_array_init(&cmcf->namespaces, cf->pool, 1,
                       sizeof(ngx_http_cache_purge_namespace_t *)) != NGX_OK)
    {
        return NULL;
    }

    cmcf->paths = &cf->cycle->paths;
    cmcf->background_purge    = NGX_CONF_UNSET;
    cmcf->queue_size          = NGX_CONF_UNSET_UINT;
    cmcf->batch_size          = NGX_CONF_UNSET_UINT;
    cmcf->throttle_ms         = NGX_CONF_UNSET_MSEC;
    cmcf->vary_aware          = NGX_CONF_UNSET;

    return cmcf;
}


static char *
ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_cache_purge_main_conf_t  *cmcf = conf;
    ngx_str_t                          name = ngx_string("cache_purge_queue");
    ngx_list_part_t                   *part;
    ngx_shm_zone_t                    *zones, *old_zone;
    ngx_uint_t                         i;
    size_t                             shm_size;
    size_t                             stride, metadata, bytes;
    ngx_path_t                       **paths;

    ngx_conf_init_value(cmcf->background_purge,    0);
    ngx_conf_init_uint_value(cmcf->queue_size,
                             NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT);
    ngx_conf_init_uint_value(cmcf->batch_size,
                             NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT);
    ngx_conf_init_msec_value(cmcf->throttle_ms,
                             NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT);
    /* Default off: vary-aware walk adds cost; opt in explicitly */
    ngx_conf_init_value(cmcf->vary_aware,          0);

    /* The capacity and batch size are positive task limits. */
    if (cmcf->queue_size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: cache_purge_queue_size must be "
                           "greater than 0");
        return NGX_CONF_ERROR;
    }

    if (cmcf->batch_size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: cache_purge_batch_size must be "
                           "greater than 0");
        return NGX_CONF_ERROR;
    }

    if (cmcf->batch_size > NGX_CACHE_PURGE_BATCH_SIZE_MAX) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "ngx_cache_purge: cache_purge_batch_size %ui "
                           "exceeds %d, using %d",
                           cmcf->batch_size, NGX_CACHE_PURGE_BATCH_SIZE_MAX,
                           NGX_CACHE_PURGE_BATCH_SIZE_MAX);
        cmcf->batch_size = NGX_CACHE_PURGE_BATCH_SIZE_MAX;
    }

    if (cmcf->throttle_ms == 0
        || cmcf->throttle_ms > (ngx_msec_t) -1 / 20)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: cache_purge_throttle_ms %M is "
                           "out of range, "
                           "expected 1 to %M ms", cmcf->throttle_ms,
                           (ngx_msec_t) -1 / 20);
        return NGX_CONF_ERROR;
    }

    old_zone = NULL;
    if (cf->cycle->old_cycle != NULL) {
        part = &cf->cycle->old_cycle->shared_memory.part;
        while (part != NULL && old_zone == NULL) {
            zones = part->elts;
            for (i = 0; i < part->nelts; i++) {
                if (zones[i].tag == &ngx_http_cache_purge_module
                    && zones[i].shm.name.len == name.len
                    && ngx_memcmp(zones[i].shm.name.data, name.data,
                                  name.len) == 0)
                {
                    old_zone = &zones[i];
                    break;
                }
            }
            part = part->next;
        }
    }

    /* Existing tasks still drain after new tasks are disabled. */
    if (!cmcf->background_purge && cmcf->namespaces.nelts == 0 && old_zone == NULL
        && cmcf->queue_status_locations.nelts == 0)
    {
        return NGX_CONF_OK;
    }

    /*
     * Queue capacity counts pending and assigned allocations together.
     * Reserve two item/key allowances per credit for slab rounding and
     * variable inline keys/pool names.  Each cache path also reserves its
     * record/name plus a cache-line-aligned stats slot per process, doubled
     * for reload metadata.  Round the total to native pages below and keep
     * at least NGX_CACHE_PURGE_SHM_MIN_PAGES for slab metadata.
     *
     * This is a conservative native-layout estimate, not a portable bytes-
     * per-task ABI.  Long keys, pool names and reload protection records can
     * exhaust the slab before the configured task count.  Namespace zones,
     * request pools, traversal buffers and thread stacks are separate.
     */
    stride = 2 * (sizeof(ngx_http_cache_purge_queue_item_t)
                  + 2 * (NGX_CACHE_PURGE_KEY_MAX_LEN + 1));

    if (cmcf->queue_size > ((size_t) -1
                            - (ngx_pagesize - 1)
                            - sizeof(ngx_http_cache_purge_queue_t)) / stride)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: cache_purge_queue_size %ui "
                           "overflows shared "
                           "memory size calculation", cmcf->queue_size);
        return NGX_CONF_ERROR;
    }

    shm_size = sizeof(ngx_http_cache_purge_queue_t)
             + cmcf->queue_size * stride;
    metadata = 0;
    paths = cmcf->paths->elts;
    for (i = 0; i < cmcf->paths->nelts; i++) {
        if (paths[i]->manager == NULL || paths[i]->loader == NULL) {
            continue;
        }
        bytes = ngx_align(sizeof(ngx_http_cache_purge_stats_t),
                          ngx_max(ngx_cacheline_size, 64)) * NGX_MAX_PROCESSES
                + ngx_max(ngx_cacheline_size, 64)
                + sizeof(ngx_http_cache_purge_path_t) + paths[i]->name.len + 1;
        if (bytes > ((size_t) -1 - metadata) / 2) {
            return NGX_CONF_ERROR;
        }
        metadata += 2 * bytes;
    }
    if (metadata > (size_t) -1 - (ngx_pagesize - 1)
        || shm_size > (size_t) -1 - metadata - (ngx_pagesize - 1))
    {
        return NGX_CONF_ERROR;
    }
    shm_size += metadata;

    /*
     * Allow for slab metadata and page alignment, even at a capacity of
     * one task.  The page size is native to the host.
     */
    shm_size = ngx_align(shm_size, ngx_pagesize);

    if (shm_size < NGX_CACHE_PURGE_SHM_MIN_PAGES * ngx_pagesize) {
        shm_size = NGX_CACHE_PURGE_SHM_MIN_PAGES * ngx_pagesize;
    }

    /* nginx replaces differently sized zones, abandoning their backlog. */
    if (old_zone != NULL && shm_size != old_zone->shm.size) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: cache_purge_queue_size changes "
                           "the shared zone "
                           "size from %uz to %uz bytes, which would abandon "
                           "queued purges; drain the queue and restart nginx",
                           old_zone->shm.size, shm_size);
        return NGX_CONF_ERROR;
    }

    cmcf->shm_zone = ngx_shared_memory_add(cf, &name, shm_size,
                                           &ngx_http_cache_purge_module);
    if (cmcf->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    cmcf->shm_zone->init = ngx_http_cache_purge_init_shm_zone;
    cmcf->shm_zone->data = cmcf;

    return NGX_CONF_OK;
}

/* Reject paths that do not name a registered cache. */
static ngx_int_t
ngx_http_cache_purge_postconfiguration(ngx_conf_t *cf)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_loc_conf_t  **locations;
    ngx_path_t                       **paths;
    ngx_str_t                         *name;
    ngx_uint_t                         i, j;
    size_t                             len;
    ngx_http_core_main_conf_t          *core;
    ngx_http_handler_pt                *handler;
    ngx_list_part_t                    *part;
    ngx_shm_zone_t                     *zones;
    ngx_http_cache_purge_namespace_t  **namespaces;
    size_t                             prefix;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);
    locations = cmcf->queue_status_locations.elts;
    paths = cmcf->paths->elts;
    namespaces = cmcf->namespaces.elts;
    prefix = sizeof("cache_purge_namespace:") - 1;
    if (cf->cycle->old_cycle != NULL) {
        part = &cf->cycle->old_cycle->shared_memory.part;
        for ( ;; part = part->next) {
            zones = part->elts;
            for (i = 0; i < part->nelts; i++) {
                if (zones[i].tag != &ngx_http_cache_purge_module
                    || zones[i].shm.name.len <= prefix
                    || ngx_memcmp(zones[i].shm.name.data,
                                  "cache_purge_namespace:", prefix))
                {
                    continue;
                }
                for (j = 0; j < cmcf->namespaces.nelts; j++) {
                    if (namespaces[j]->zone == &zones[i]
                        || (namespaces[j]->zone->shm.name.len == zones[i].shm.name.len
                            && ngx_memcmp(namespaces[j]->zone->shm.name.data,
                                          zones[i].shm.name.data, zones[i].shm.name.len) == 0))
                    {
                        break;
                    }
                }
                if (j == cmcf->namespaces.nelts) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "ngx_cache_purge: removing namespace shared zone "
                        "\"%V\" requires a full restart", &zones[i].shm.name);
                    return NGX_ERROR;
                }
            }
            if (part->next == NULL) {
                break;
            }
        }
    }

    for (i = 0; i < cmcf->queue_status_locations.nelts; i++) {
        name = &locations[i]->queue_status_cache_path;

        for (j = 0; j < cmcf->paths->nelts; j++) {
            if (paths[j]->manager == NULL || paths[j]->loader == NULL) {
                continue;
            }

            len = ngx_http_cache_purge_path_length(&paths[j]->name);
            if (name->len == len
                && ngx_http_cache_purge_path_equal(name->data,
                                                  paths[j]->name.data, len))
            {
                break;
            }
        }

        if (j == cmcf->paths->nelts) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "ngx_cache_purge: cache_purge_queue_status: "
                               "unknown cache "
                               "path \"%V\"", name);
            return NGX_ERROR;
        }
    }

    if (cmcf->shm_zone != NULL) {
        core = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
        handler = ngx_array_push(&core->phases[NGX_HTTP_LOG_PHASE].handlers);
        if (handler == NULL) {
            return NGX_ERROR;
        }
        *handler = ngx_http_cache_purge_log;
# if (NGX_HTTP_PROXY)
        handler = ngx_array_push(&core->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
        if (handler == NULL) {
            return NGX_ERROR;
        }
        *handler = ngx_http_cache_refresh_guard;
# endif
    }
    return NGX_OK;
}


/*
 * Reattach the existing zone on reload or in a Windows worker.
 * All shared edits hold the zone lock; slab calls expect it to be held.
 * Dead-worker lock release does not repair an incomplete list edit.
 */

/*
 * A missing file is an expected race with another worker or the cache
 * manager.  Report other deletion errors.
 */
/*
 * Workers cache their clocks independently.  Signed elapsed time handles
 * rollover and clamps a slightly newer timestamp to zero.
 */
/* Install the same path protection for every directory walk. */

/* Bounded recovery; persistent nodes are inspected only under the zone lock. */

/* Shared lexical path comparisons. */


/* -- path metadata ------------------------------------------------------ */

/* The zone lock is held.  Names and records persist to restart. */
/* -- JSON endpoint ------------------------------------------------------ */

static char *
ngx_http_cache_purge_queue_status_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_loc_conf_t   *cplcf = conf;
    ngx_http_cache_purge_loc_conf_t  **location;
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_core_loc_conf_t          *clcf;
    ngx_str_t                         *value;

    (void) cmd;

    if (cplcf->queue_status_cache_path.data != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;
    if (value[1].len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "requires a non-empty cache path", &cmd->name);
        return NGX_CONF_ERROR;
    }

    cplcf->queue_status_cache_path = value[1];
    if (ngx_conf_full_name(cf->cycle, &cplcf->queue_status_cache_path, 0)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }
    cplcf->queue_status_cache_path.len =
        ngx_http_cache_purge_path_length(&cplcf->queue_status_cache_path);

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);
    location = ngx_array_push(&cmcf->queue_status_locations);
    if (location == NULL) {
        return NGX_CONF_ERROR;
    }
    *location = cplcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_cache_purge_queue_status_handler;

    return NGX_CONF_OK;
}

/* Approximate snapshot: no mutex and no atomic read-modify-write operations. */


/* -- file-walk helpers -------------------------------------------------- */

/* Compare complete path components; no trailing zero is needed. */


/* -- FastCGI ------------------------------------------------------------ */

# if (NGX_HTTP_FASTCGI)
char *
ngx_http_fastcgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_fastcgi_loc_conf_t      *flcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t  cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->fastcgi.enable != NGX_CONF_UNSET) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->fastcgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "(separate location syntax) is not allowed here",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    flcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_fastcgi_module);

#  if (nginx_version >= 1007009)
    if (flcf->upstream.cache > 0)
#  else
    if (flcf->upstream.cache != NGX_CONF_UNSET_PTR
        && flcf->upstream.cache != NULL)
#  endif
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"fastcgi_cache\"",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (flcf->upstream.upstream || flcf->fastcgi_lengths) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"fastcgi_pass\"",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (flcf->upstream.store > 0
#  if (nginx_version < 1007009)
        || flcf->upstream.store_lengths
#  endif
       )
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"fastcgi_store\"",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

#  if (nginx_version >= 1007009)
    flcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
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
#  endif

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &flcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->fastcgi.enable = 0;
    cplcf->conf           = &cplcf->fastcgi;
    clcf->handler         = ngx_http_fastcgi_cache_purge_handler;

    return NGX_CONF_OK;
}


# endif /* NGX_HTTP_FASTCGI */


/* -- Proxy -------------------------------------------------------------- */

# if (NGX_HTTP_PROXY)
char *
ngx_http_proxy_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_proxy_loc_conf_t        *plcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t  cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->proxy.enable != NGX_CONF_UNSET) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->proxy);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "(separate location syntax) is not allowed here",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_proxy_module);

#  if (nginx_version >= 1007009)
    if (plcf->upstream.cache > 0)
#  else
    if (plcf->upstream.cache != NGX_CONF_UNSET_PTR
        && plcf->upstream.cache != NULL)
#  endif
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"proxy_cache\"", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (plcf->upstream.upstream || plcf->proxy_lengths) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"proxy_pass\"", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (plcf->upstream.store > 0
#  if (nginx_version < 1007009)
        || plcf->upstream.store_lengths
#  endif
       )
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"proxy_store\"", &cmd->name);
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    /*
     * Store the zone and key in this location's own settings.  Altering
     * core proxy cache state here also changes core location merging.
     */
#  if (nginx_version >= 1007009)
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
    ccv.complex_value = &cv;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (cv.lengths != NULL) {
        /* dynamic zone expression -- allocate a persistent copy */
        cplcf->proxy_separate_value = ngx_palloc(cf->pool,
                                          sizeof(ngx_http_complex_value_t));
        if (cplcf->proxy_separate_value == NULL) {
            return NGX_CONF_ERROR;
        }
        *cplcf->proxy_separate_value = cv;
    } else {
        /* static zone name -- look it up in shared memory table */
        cplcf->proxy_separate_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                         &ngx_http_proxy_module);
        if (cplcf->proxy_separate_zone == NULL) {
            return NGX_CONF_ERROR;
        }
    }
#  else
    /* nginx < 1.7.9: cache is just a shm_zone pointer */
    cplcf->proxy_separate_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                      &ngx_http_proxy_module);
    if (cplcf->proxy_separate_zone == NULL) {
        return NGX_CONF_ERROR;
    }
#  endif

    /* compile the purge-key template into our own field */
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &cplcf->proxy_separate_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->proxy.enable = 0;
    cplcf->conf         = &cplcf->proxy;
    clcf->handler       = ngx_http_proxy_cache_purge_handler;

    return NGX_CONF_OK;
}


# endif /* NGX_HTTP_PROXY */


/* -- SCGI --------------------------------------------------------------- */

# if (NGX_HTTP_SCGI)
char *
ngx_http_scgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_scgi_loc_conf_t         *slcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t  cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->scgi.enable != NGX_CONF_UNSET) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->scgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "(separate location syntax) is not allowed here",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    slcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_scgi_module);

#  if (nginx_version >= 1007009)
    if (slcf->upstream.cache > 0)
#  else
    if (slcf->upstream.cache != NGX_CONF_UNSET_PTR
        && slcf->upstream.cache != NULL)
#  endif
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"scgi_cache\"", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (slcf->upstream.upstream || slcf->scgi_lengths) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"scgi_pass\"", &cmd->name);
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

#  if (nginx_version >= 1007009)
    slcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
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
#  endif

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &slcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->scgi.enable = 0;
    cplcf->conf        = &cplcf->scgi;
    clcf->handler      = ngx_http_scgi_cache_purge_handler;

    return NGX_CONF_OK;
}


# endif /* NGX_HTTP_SCGI */


/* -- uWSGI -------------------------------------------------------------- */

# if (NGX_HTTP_UWSGI)
char *
ngx_http_uwsgi_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_compile_complex_value_t  ccv;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_http_uwsgi_loc_conf_t        *ulcf;
    ngx_str_t                        *value;
#  if (nginx_version >= 1007009)
    ngx_http_complex_value_t  cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->uwsgi.enable != NGX_CONF_UNSET) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts != 3) {
        return ngx_http_cache_purge_conf(cf, &cplcf->uwsgi);
    }

    if (cf->cmd_type & (NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF)) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "(separate location syntax) is not allowed here",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    ulcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_uwsgi_module);

#  if (nginx_version >= 1007009)
    if (ulcf->upstream.cache > 0)
#  else
    if (ulcf->upstream.cache != NGX_CONF_UNSET_PTR
        && ulcf->upstream.cache != NULL)
#  endif
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"uwsgi_cache\"", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (ulcf->upstream.upstream || ulcf->uwsgi_lengths) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is incompatible with \"uwsgi_pass\"", &cmd->name);
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

#  if (nginx_version >= 1007009)
    ulcf->upstream.cache = 1;

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[1];
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
#  endif

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf            = cf;
    ccv.value         = &value[2];
    ccv.complex_value = &ulcf->cache_key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    cplcf->uwsgi.enable = 0;
    cplcf->conf         = &cplcf->uwsgi;
    clcf->handler       = ngx_http_uwsgi_cache_purge_handler;

    return NGX_CONF_OK;
}


# endif /* NGX_HTTP_UWSGI */


/* -- response type directive -------------------------------------------- */

char *
ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_loc_conf_t  *cplcf = conf;
    ngx_str_t                        *value;

    (void) cmd;

    if (cplcf->response_type != NGX_CONF_UNSET_UINT) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "is duplicate", &cmd->name);
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts != 2) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: \"%V\" directive "
                           "requires exactly one argument: html|json|xml|text|none",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "html") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_HTML;
    } else if (ngx_strcmp(value[1].data, "json") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_JSON;
    } else if (ngx_strcmp(value[1].data, "xml") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_XML;
    } else if (ngx_strcmp(value[1].data, "text") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT;
    } else if (ngx_strcmp(value[1].data, "none") == 0) {
        cplcf->response_type = NGX_CACHE_PURGE_RESPONSE_TYPE_NONE;
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "ngx_cache_purge: invalid parameter \"%V\", expected "
            "html|json|xml|text|none",
            &value[1]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/* -- access control ----------------------------------------------------- */


/* -- response builder --------------------------------------------------- */

/* Count extra bytes or write the key in its response representation. */


/* -- cache get helper (nginx >= 1.7.9) --------------------------------- */

/* -- configuration parser ----------------------------------------------- */

char *
ngx_http_cache_purge_conf(ngx_conf_t *cf, ngx_http_cache_purge_conf_t *cpcf)
{
    ngx_cidr_t      cidr;
    ngx_in_cidr_t  *access;
# if (NGX_HAVE_INET6)
    ngx_in6_cidr_t  *access6;
# endif
    ngx_str_t  *value;
    ngx_int_t   rc;
    ngx_uint_t  i, from_position;

    from_position = 2;
    value         = cf->args->elts;

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

    if (ngx_strcmp(value[from_position].data, "purge_all") == 0) {
        cpcf->purge_all = 1;
        from_position++;
    }

    if (ngx_strcmp(value[from_position].data, "from") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "ngx_cache_purge: invalid parameter \"%V\", expected \"from\" "
            "keyword",
            &value[from_position]);
        return NGX_CONF_ERROR;
    }

    if (from_position + 1 == cf->args->nelts) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "ngx_cache_purge: missing address after \"from\"");
        return NGX_CONF_ERROR;
    }

    if (ngx_strcmp(value[from_position + 1].data, "all") == 0) {
        if (from_position + 2 != cf->args->nelts) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "ngx_cache_purge: invalid parameter \"%V\" "
                               "after \"all\"",
                               &value[from_position + 2]);
            return NGX_CONF_ERROR;
        }
        cpcf->enable = 1;
        return NGX_CONF_OK;
    }

    for (i = from_position + 1; i < cf->args->nelts; i++) {
        rc = ngx_ptocidr(&value[i], &cidr);

        if (rc == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "ngx_cache_purge: invalid parameter \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                "ngx_cache_purge: low address bits of %V are meaningless",
                &value[i]);
        }

        switch (cidr.family) {
        case AF_INET:
            if (cpcf->access == NULL) {
                cpcf->access = ngx_array_create(cf->pool,
                    cf->args->nelts - (from_position + 1),
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
                cpcf->access6 = ngx_array_create(cf->pool,
                    cf->args->nelts - (from_position + 1),
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
# endif
        }
    }

    cpcf->enable = 1;

    return NGX_CONF_OK;
}


/* -- location configuration --------------------------------------------- */

# if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
static void
ngx_http_cache_purge_merge_conf(ngx_http_cache_purge_conf_t *conf,
    ngx_http_cache_purge_conf_t *prev)
{
    if (conf->enable == NGX_CONF_UNSET) {
        if (prev->enable == 1) {
            conf->enable    = prev->enable;
            conf->method    = prev->method;
            conf->purge_all = prev->purge_all;
            conf->access    = prev->access;
            conf->access6   = prev->access6;
        } else {
            conf->enable = 0;
        }
    }
}
# endif

void *
ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_cache_purge_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

# if (NGX_HTTP_PROXY)
    conf->refresh_timeout = NGX_CONF_UNSET_MSEC;
# endif

    /*
     * set by ngx_pcalloc():
     *   conf->*.method         = { 0, NULL }
     *   conf->*.access         = NULL
     *   conf->*.access6        = NULL
     *   conf->handler          = NULL
     *   conf->original_handler = NULL
     */

# if (NGX_HTTP_FASTCGI)
    conf->fastcgi.enable = NGX_CONF_UNSET;
# endif
# if (NGX_HTTP_PROXY)
    conf->proxy.enable   = NGX_CONF_UNSET;
# endif
# if (NGX_HTTP_SCGI)
    conf->scgi.enable    = NGX_CONF_UNSET;
# endif
# if (NGX_HTTP_UWSGI)
    conf->uwsgi.enable   = NGX_CONF_UNSET;
# endif

    conf->response_type = NGX_CONF_UNSET_UINT;
    conf->conf     = NGX_CONF_UNSET_PTR;

    return conf;
}


char *
ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_cache_purge_loc_conf_t   *prev = parent;
    ngx_http_cache_purge_loc_conf_t   *conf = child;
    ngx_http_core_loc_conf_t          *clcf;
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_loc_conf_t  **location;
    ngx_flag_t                         purge_set;
    /*
     * Save the local handler for explicit directives and named locations.
     * An if-block has no local handler: inherit the saved parent handler
     * instead, so ordinary HTTP traffic still reaches its content handler.
     * The method gate remains installed in both cases.
     */
# if (NGX_HTTP_FASTCGI)
    ngx_flag_t  was_set_fastcgi;
# endif
# if (NGX_HTTP_PROXY)
    ngx_flag_t  was_set_proxy;
# endif
# if (NGX_HTTP_SCGI)
    ngx_flag_t  was_set_scgi;
# endif
# if (NGX_HTTP_UWSGI)
    ngx_flag_t  was_set_uwsgi;
# endif

    if (conf->namespace == NULL) {
        conf->namespace = prev->namespace;
        conf->logical_key = prev->logical_key;
    }

# if (NGX_HTTP_PROXY)
    if (conf->proxy.enable == NGX_CONF_UNSET
        && conf->proxy_separate_zone == NULL
        && conf->proxy_separate_value == NULL)
    {
        conf->proxy_separate_zone  = prev->proxy_separate_zone;
        conf->proxy_separate_value = prev->proxy_separate_value;
        conf->proxy_separate_key   = prev->proxy_separate_key;
        conf->refresh             = prev->refresh;
    }
    ngx_conf_merge_msec_value(conf->refresh_timeout, prev->refresh_timeout, 10000);
    if (conf->refresh_location.data == NULL) {
        conf->refresh_location = prev->refresh_location;
        conf->refresh_prefix = prev->refresh_prefix;
    }
    if (conf->refresh && conf->namespace == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "ngx_cache_purge: \"proxy_cache_refresh\" requires "
                          "\"cache_purge_key\"");
        return NGX_CONF_ERROR;
    }
    if (conf->refresh && conf->refresh_location.data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "ngx_cache_purge: \"proxy_cache_refresh\" requires "
                          "\"cache_purge_refresh_profile\"");
        return NGX_CONF_ERROR;
    }
    if (conf->refresh && (conf->refresh_timeout == 0
                         || conf->refresh_timeout > 3600000))
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "ngx_cache_purge: \"cache_purge_refresh_timeout\" "
                          "%M ms is out of range, expected 1 to 3600000 ms",
                          conf->refresh_timeout);
        return NGX_CONF_ERROR;
    }
# endif

    ngx_conf_merge_str_value(conf->thread_pool_name,
                             prev->thread_pool_name, "default");
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);

# if (NGX_HTTP_FASTCGI)
    was_set_fastcgi = (conf->fastcgi.enable == 1);
# endif
# if (NGX_HTTP_PROXY)
    was_set_proxy   = (conf->proxy.enable   == 1);
# endif
# if (NGX_HTTP_SCGI)
    was_set_scgi    = (conf->scgi.enable    == 1);
# endif
# if (NGX_HTTP_UWSGI)
    was_set_uwsgi   = (conf->uwsgi.enable   == 1);
# endif

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    ngx_conf_merge_uint_value(conf->response_type, prev->response_type,
                              NGX_CACHE_PURGE_RESPONSE_TYPE_JSON);

    purge_set = (conf->conf != NGX_CONF_UNSET_PTR);
# if (NGX_HTTP_FASTCGI)
    purge_set |= was_set_fastcgi;
# endif
# if (NGX_HTTP_PROXY)
    purge_set |= was_set_proxy;
# endif
# if (NGX_HTTP_SCGI)
    purge_set |= was_set_scgi;
# endif
# if (NGX_HTTP_UWSGI)
    purge_set |= was_set_uwsgi;
# endif

    if (conf->queue_status_cache_path.data != NULL) {
        if (conf->conf != NGX_CONF_UNSET_PTR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "ngx_cache_purge: "
                               "cache_purge_queue_status conflicts with a "
                               "purge handler");
            return NGX_CONF_ERROR;
        }
# if (NGX_HTTP_FASTCGI)
        if (was_set_fastcgi) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "ngx_cache_purge: "
                               "cache_purge_queue_status conflicts with "
                               "fastcgi_cache_purge");
            return NGX_CONF_ERROR;
        }
# endif
# if (NGX_HTTP_PROXY)
        if (was_set_proxy) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "ngx_cache_purge: "
                               "cache_purge_queue_status conflicts with "
                               "proxy_cache_purge");
            return NGX_CONF_ERROR;
        }
# endif
# if (NGX_HTTP_SCGI)
        if (was_set_scgi) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "ngx_cache_purge: "
                               "cache_purge_queue_status conflicts with "
                               "scgi_cache_purge");
            return NGX_CONF_ERROR;
        }
# endif
# if (NGX_HTTP_UWSGI)
        if (was_set_uwsgi) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "ngx_cache_purge: "
                               "cache_purge_queue_status conflicts with "
                               "uwsgi_cache_purge");
            return NGX_CONF_ERROR;
        }
# endif
    }

    if (clcf->handler == ngx_http_cache_purge_queue_status_handler
        || (clcf->handler == NULL && prev->queue_status_cache_path.data != NULL
            && !purge_set))
    {
        if (conf->queue_status_cache_path.data == NULL) {
            conf->queue_status_cache_path = prev->queue_status_cache_path;
            cmcf = ngx_http_conf_get_module_main_conf(cf,
                                                  ngx_http_cache_purge_module);
            location = ngx_array_push(&cmcf->queue_status_locations);
            if (location == NULL) {
                return NGX_CONF_ERROR;
            }
            *location = conf;
        }
        clcf->handler = ngx_http_cache_purge_queue_status_handler;
        return NGX_CONF_OK;
    }

# if (NGX_HTTP_PROXY)
    if (conf->refresh
        && (clcf->handler == NULL
            || clcf->handler == ngx_http_cache_refresh_handler))
    {
        /* Preserve the explicit method gate through anonymous if locations;
         * inherited inline PURGE settings must not replace this capability. */
        conf->proxy.enable = 0;
        conf->conf = &conf->proxy;
        clcf->handler = ngx_http_cache_refresh_handler;
        goto register_pool;
    }
# endif

# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_merge_conf(&conf->fastcgi, &prev->fastcgi);

    if (conf->fastcgi.enable) {
        conf->conf             = &conf->fastcgi;
        conf->handler          = ngx_http_fastcgi_cache_purge_handler;
        conf->original_handler = (was_set_fastcgi || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        goto register_pool;
    }
# endif

# if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_merge_conf(&conf->proxy, &prev->proxy);

    if (conf->proxy.enable) {
        conf->conf             = &conf->proxy;
        conf->handler          = ngx_http_proxy_cache_purge_handler;
        conf->original_handler = (was_set_proxy || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        goto register_pool;
    }
# endif

# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_merge_conf(&conf->scgi, &prev->scgi);

    if (conf->scgi.enable) {
        conf->conf             = &conf->scgi;
        conf->handler          = ngx_http_scgi_cache_purge_handler;
        conf->original_handler = (was_set_scgi || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        goto register_pool;
    }
# endif

# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_merge_conf(&conf->uwsgi, &prev->uwsgi);

    if (conf->uwsgi.enable) {
        conf->conf             = &conf->uwsgi;
        conf->handler          = ngx_http_uwsgi_cache_purge_handler;
        conf->original_handler = (was_set_uwsgi || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        goto register_pool;
    }
# endif

    ngx_conf_merge_ptr_value(conf->conf, prev->conf, NULL);

    if (conf->handler == NULL) {
        conf->handler = prev->handler;
    }

    if (conf->original_handler == NULL) {
        conf->original_handler = prev->original_handler;
    }

# if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
register_pool:
# endif
# if (NGX_THREADS && !NGX_WIN32)
    if ((cmcf->background_purge || conf->namespace != NULL)
        && conf->conf != NULL
        && ngx_thread_pool_add(cf, &conf->thread_pool_name) == NULL)
    {
        return NGX_CONF_ERROR;
    }
# endif
    return NGX_CONF_OK;
}


#else /* !NGX_HTTP_CACHE */

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    NULL, NULL,   /* pre/postconfiguration  */
    NULL, NULL,   /* create/init main conf  */
    NULL, NULL,   /* create/merge srv conf  */
    NULL, NULL    /* create/merge loc conf  */
};

ngx_module_t  ngx_http_cache_purge_module = {
    NGX_MODULE_V1,
    &ngx_http_cache_purge_module_ctx,
    NULL,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

#endif /* NGX_HTTP_CACHE */
