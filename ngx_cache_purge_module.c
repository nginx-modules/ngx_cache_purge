/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * Copyright (C) 2016-2026 Denis Denisov
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

#if (NGX_HTTP_PROXY) && (nginx_version >= 1029004)
#include <ngx_http_proxy_module.h>
#endif


#ifndef nginx_version
# error This module cannot be built against an unknown nginx version.
#endif

#define NGX_CACHE_PURGE_RESPONSE_TYPE_HTML  1
#define NGX_CACHE_PURGE_RESPONSE_TYPE_XML   2
#define NGX_CACHE_PURGE_RESPONSE_TYPE_JSON  3
#define NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT  4

#define NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT   1024
#define NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT   10
/*
 * This constant is assigned directly to an ngx_msec_t field via
 * ngx_conf_init_msec_value() -- it bypasses ngx_parse_time() and is
 * therefore in raw milliseconds.  The corresponding directive,
 * cache_purge_throttle_ms, is parsed by ngx_conf_set_msec_slot which
 * calls ngx_parse_time(value, 0): bare integers are treated as seconds
 * per the nginx time-value contract, so operators must write an explicit
 * suffix ("10ms", "1s", ...) to get the intended unit.
 */
#define NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT  10  /* milliseconds */
#define NGX_CACHE_PURGE_KEY_MAX_LEN          512
#define NGX_CACHE_PURGE_QUEUE_TIMEOUT        60000   /* ms */
/*
 * Byte offset from the start of a cache file to the first character of the
 * cached key string.  The nginx cache file layout is:
 *
 *   [ ngx_http_file_cache_header_t ][ "\nKEY: " ][ <key> ][ "\n" ]...
 *
 * sizeof(ngx_http_file_cache_header_t) skips the binary header.
 * NGX_CACHE_PURGE_KEY_HDR_OFFSET (6) accounts for the literal prefix
 * "\nKEY: " (newline + 'K' + 'E' + 'Y' + ':' + ' ' = 6 bytes).
 *
 * This layout has been stable since nginx 0.7.x.  If it ever changes, only
 * this constant and its comment need updating.
 */
#define NGX_CACHE_PURGE_KEY_HDR_OFFSET       6

/*
 * Minimum shared-memory size for the background queue, expressed in pages.
 * The slab allocator consumes an amount of metadata (pool header, slot
 * descriptors, stat entries, page descriptors, and an alignment gap) that
 * varies with nginx version, build flags, and architecture.  Rather than
 * attempting to compute that overhead from internal slab structs -- which
 * differ between nginx 1.8/1.9+, are affected by NGX_HAVE_POSIX_SEM /
 * --with-threads, and scale with the OS page size (4 KB on x86 Linux,
 * 8-64 KB on some *BSD / ARM / POWER platforms) -- we simply enforce a
 * floor of 8 pages.  ngx_pagesize is the runtime value, so the minimum
 * scales automatically on big-page architectures.  8 pages is generous
 * enough to accommodate all slab metadata overhead while leaving several
 * full pages of usable heap even for queue_size=1.
 */
#define NGX_CACHE_PURGE_SHM_MIN_PAGES        8


static const char ngx_http_cache_purge_content_type_json[] = "application/json";
static const char ngx_http_cache_purge_content_type_html[] = "text/html";
static const char ngx_http_cache_purge_content_type_xml[]  = "text/xml";
static const char ngx_http_cache_purge_content_type_text[] = "text/plain";

static const size_t ngx_http_cache_purge_content_type_json_size =
    sizeof(ngx_http_cache_purge_content_type_json);
static const size_t ngx_http_cache_purge_content_type_html_size =
    sizeof(ngx_http_cache_purge_content_type_html);
static const size_t ngx_http_cache_purge_content_type_xml_size =
    sizeof(ngx_http_cache_purge_content_type_xml);
static const size_t ngx_http_cache_purge_content_type_text_size =
    sizeof(ngx_http_cache_purge_content_type_text);

static const char ngx_http_cache_purge_body_templ_json[] =
    "{\"Key\": \"%s\", \"Status\": \"%s\"}";
static const char ngx_http_cache_purge_body_templ_html[] =
    "<html><head><title>Cache Purge</title></head>"
    "<body bgcolor=\"white\"><center><h1>Cache Purge</h1>"
    "<p>Key: %s</p><p>Status: %s</p></center></body></html>";
static const char ngx_http_cache_purge_body_templ_xml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<status><Key><![CDATA[%s]]></Key><Status>%s</Status></status>";
static const char ngx_http_cache_purge_body_templ_text[] =
    "Key: %s\nStatus: %s\n";

static const size_t ngx_http_cache_purge_body_templ_json_size =
    sizeof(ngx_http_cache_purge_body_templ_json);
static const size_t ngx_http_cache_purge_body_templ_html_size =
    sizeof(ngx_http_cache_purge_body_templ_html);
static const size_t ngx_http_cache_purge_body_templ_xml_size =
    sizeof(ngx_http_cache_purge_body_templ_xml);
static const size_t ngx_http_cache_purge_body_templ_text_size =
    sizeof(ngx_http_cache_purge_body_templ_text);


#if (NGX_HTTP_CACHE)

/* -- forward declarations ----------------------------------------------- */

typedef struct ngx_http_cache_purge_queue_item_s ngx_http_cache_purge_queue_item_t;
typedef struct ngx_http_cache_purge_queue_s      ngx_http_cache_purge_queue_t;
typedef struct ngx_http_cache_purge_main_conf_s  ngx_http_cache_purge_main_conf_t;


typedef struct ngx_http_cache_purge_path_s    ngx_http_cache_purge_path_t;
typedef struct ngx_http_cache_purge_status_s  ngx_http_cache_purge_status_t;


/* -- data structures ---------------------------------------------------- */

/* Immutable name and counters share the queue zone's lifetime and mutex. */
struct ngx_http_cache_purge_path_s {
    ngx_str_t                     name;
    ngx_uint_t                    waiting;
    ngx_uint_t                    purge_all;
    ngx_uint_t                    active;
    ngx_http_cache_purge_path_t  *next;
};

/* Configuration-owned binding; shared counters are resolved before fork. */
struct ngx_http_cache_purge_status_s {
    ngx_str_t                       name;
    ngx_http_cache_purge_path_t    *path;
    ngx_http_cache_purge_status_t  *next;
};

struct ngx_http_cache_purge_queue_item_s {
    ngx_str_t                           cache_path;
    ngx_str_t                           key_partial;
    ngx_uint_t                          hash;
    ngx_flag_t                          purge_all;
    ngx_http_cache_purge_path_t        *path;
    ngx_msec_t                          enqueued_at;
    ngx_http_cache_purge_queue_item_t  *next;
};

struct ngx_http_cache_purge_queue_s {
    ngx_http_cache_purge_queue_item_t  *head;
    ngx_http_cache_purge_queue_item_t  *tail;
    /* All queue state is protected by shpool->mutex. */
    ngx_uint_t                          size;
    ngx_http_cache_purge_path_t        *paths;
    ngx_slab_pool_t                    *shpool;
    ngx_uint_t                          max_size;
};

struct ngx_http_cache_purge_main_conf_s {
    ngx_http_cache_purge_queue_t   *queue;
    ngx_shm_zone_t                 *shm_zone;
    ngx_http_cache_purge_status_t  *statuses;
    ngx_uint_t                      queue_size;
    ngx_uint_t                      batch_size;
    ngx_msec_t                      throttle_ms;
    ngx_flag_t                      background_purge;
    ngx_flag_t                      legacy_status_codes;
    /*
     * vary_aware: when on, an exact-key purge walks the cache directory after
     * deleting the primary file and removes any remaining files that carry the
     * same KEY: string (i.e. Vary / gzip_vary variants at different paths).
     * Disabled by default because it adds a full cache walk per purge request.
     */
    ngx_flag_t                      vary_aware;
};

typedef struct {
    ngx_flag_t    enable;
    ngx_str_t     method;
    ngx_flag_t    purge_all;
    ngx_array_t  *access;    /* ngx_in_cidr_t  */
    ngx_array_t  *access6;   /* ngx_in6_cidr_t */
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

    ngx_http_cache_purge_conf_t    *conf;
    ngx_http_handler_pt          handler;
    ngx_http_handler_pt          original_handler;
    ngx_uint_t                      response_type;
    ngx_http_cache_purge_status_t  *status;

# if (NGX_HTTP_PROXY)
    /*
     * Separate-location syntax stores the cache zone and purge key here
     * instead of in plcf->upstream, which avoids triggering nginx's internal
     * proxy_cache merge path and the resulting duplicate location "/" error
     * introduced in nginx >= 1.27.x.
     */
    ngx_shm_zone_t              *proxy_separate_zone;   /* static zone name  */
    ngx_http_complex_value_t    *proxy_separate_value;  /* dynamic zone expr */
    ngx_http_complex_value_t     proxy_separate_key;    /* purge key template*/
# endif
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    u_char                 *key_partial;
    ngx_uint_t              key_len;
    u_char                  key_buffer[NGX_CACHE_PURGE_KEY_MAX_LEN];
    ngx_uint_t              files_deleted;
    ngx_uint_t              files_checked;
    size_t                  root_len;
    /*
     * cache is set by ngx_http_cache_purge_delete_variants() so that
     * delete_exact_file can update shm metadata (sh->size, node->exists,
     * node->fs_size) for each variant it deletes.  NULL in all other walk
     * contexts where metadata updates are not needed.
     */
    ngx_http_file_cache_t  *cache;
} ngx_http_cache_purge_walk_ctx_t;


/* -- function prototypes ------------------------------------------------ */

static void *ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_cache_purge_postconfiguration(ngx_conf_t *cf);
static char *ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_int_t ngx_http_cache_purge_init_worker(ngx_cycle_t *cycle);
static void ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle);
static void ngx_http_cache_purge_background_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_cache_purge_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all);
static ngx_int_t ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle);
static ngx_http_cache_purge_path_t *ngx_http_cache_purge_path(
    ngx_http_cache_purge_queue_t *queue, ngx_str_t *name);
static char *ngx_http_cache_purge_status_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_cache_purge_status_handler(ngx_http_request_t *r);
static ngx_uint_t ngx_http_cache_purge_protected(ngx_str_t *path,
    size_t root_len);
static ngx_uint_t ngx_http_cache_purge_cache_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_cache_purge_pre_tree(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_uint_t ngx_http_cache_purge_hash_key(ngx_str_t *cache_path,
    ngx_str_t *key);
static ngx_http_cache_purge_queue_item_t *ngx_http_cache_purge_find_duplicate(
    ngx_http_cache_purge_queue_t *queue, ngx_uint_t hash,
    ngx_str_t *cache_path, ngx_str_t *key, ngx_flag_t purge_all);

# if (NGX_HTTP_FASTCGI)
char      *ngx_http_fastcgi_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r);
# endif

# if (NGX_HTTP_PROXY)
char      *ngx_http_proxy_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r);
# endif

# if (NGX_HTTP_SCGI)
char      *ngx_http_scgi_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r);
# endif

# if (NGX_HTTP_UWSGI)
char      *ngx_http_uwsgi_cache_purge_conf(ngx_conf_t *cf,
               ngx_command_t *cmd, void *conf);
ngx_int_t  ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r);
# endif

char *ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_http_cache_purge_queue_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_http_cache_purge_legacy_status_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_http_cache_purge_vary_aware_conf(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_purge_file_cache_noop(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_purge_file_cache_delete_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_purge_file_cache_delete_partial_file(
    ngx_tree_ctx_t *ctx, ngx_str_t *path);
/*
 * ngx_http_purge_file_cache_delete_exact_file:
 *
 * Vary-aware exact-match handler.  Reads (key_len + 1) bytes from the
 * cache file at the KEY: offset.  Deletes the file only when:
 *   - the first key_len bytes match key_partial exactly (case-insensitive), AND
 *   - the byte immediately following the key is '\n'
 *
 * The '\n' check confirms the stored key is exactly key_len characters, so
 * keys that are longer but share a common prefix are not matched.  Because all
 * Vary variants of an entry store the same KEY: string, this walk removes every
 * variant file regardless of the filesystem path each one occupies.
 */
static ngx_int_t ngx_http_purge_file_cache_delete_exact_file(
    ngx_tree_ctx_t *ctx, ngx_str_t *path);
static void ngx_http_cache_purge_invalidate_node(ngx_http_file_cache_t *cache,
    ngx_str_t *path);

static void ngx_http_cache_purge_delete_variants(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache);

ngx_int_t  ngx_http_cache_purge_access_handler(ngx_http_request_t *r);
ngx_int_t  ngx_http_cache_purge_access(ngx_array_t *a, ngx_array_t *a6,
               struct sockaddr *s);
ngx_int_t  ngx_http_cache_purge_send_response(ngx_http_request_t *r,
               ngx_str_t *status);
# if (nginx_version >= 1007009)
ngx_int_t  ngx_http_cache_purge_cache_get(ngx_http_request_t *r,
               ngx_http_upstream_t *u, ngx_http_file_cache_t **cache);
# endif
ngx_int_t  ngx_http_cache_purge_init(ngx_http_request_t *r,
               ngx_http_file_cache_t *cache,
               ngx_http_complex_value_t *cache_key);
void       ngx_http_cache_purge_handler(ngx_http_request_t *r);
ngx_int_t  ngx_http_file_cache_purge(ngx_http_request_t *r);
void       ngx_http_cache_purge_all(ngx_http_request_t *r,
               ngx_http_file_cache_t *cache);
ngx_uint_t ngx_http_cache_purge_partial(ngx_http_request_t *r,
               ngx_http_file_cache_t *cache);
ngx_int_t  ngx_http_cache_purge_is_partial(ngx_http_request_t *r);
char      *ngx_http_cache_purge_conf(ngx_conf_t *cf,
               ngx_http_cache_purge_conf_t *cpcf);
void      *ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf);
char      *ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf,
               void *parent, void *child);


/* -- module commands ---------------------------------------------------- */

static ngx_command_t  ngx_http_cache_purge_module_commands[] = {

# if (NGX_HTTP_FASTCGI)
    { ngx_string("fastcgi_cache_purge"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_fastcgi_cache_purge_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
# endif

# if (NGX_HTTP_PROXY)
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

    { ngx_string("cache_purge_background_queue"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_queue_conf,
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

    /* Accepts standard nginx time values.  A bare integer means seconds per
     * the nginx time-value contract; use an explicit suffix for milliseconds:
     *   cache_purge_throttle_ms 10ms;   -- 10 ms  (correct)
     *   cache_purge_throttle_ms 10;     -- 10 s   (almost certainly wrong)
     * Default when directive is absent: 10 ms (set via ngx_conf_init_msec_value,
     * which bypasses the parser and assigns the raw integer directly). */
    { ngx_string("cache_purge_throttle_ms"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, throttle_ms), NULL },

    { ngx_string("cache_purge_legacy_status"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_legacy_status_conf,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, legacy_status_codes), NULL },

    { ngx_string("cache_purge_vary_aware"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_vary_aware_conf,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_main_conf_t, vary_aware), NULL },

    { ngx_string("cache_purge_status"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_cache_purge_status_conf,
      NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    ngx_null_command
};


/* -- module context & descriptor ---------------------------------------- */

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    NULL,                                   /* preconfiguration  */
    ngx_http_cache_purge_postconfiguration, /* postconfiguration */
    ngx_http_cache_purge_create_main_conf,  /* create main conf  */
    ngx_http_cache_purge_init_main_conf,    /* init main conf    */
    NULL,                                   /* create srv conf   */
    NULL,                                   /* merge srv conf    */
    ngx_http_cache_purge_create_loc_conf,   /* create loc conf   */
    ngx_http_cache_purge_merge_loc_conf     /* merge loc conf    */
};

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
    NULL,                                   /* exit master  */
    NGX_MODULE_V1_PADDING
};

/* Per-worker globals -- safe because nginx forks one process per worker */
static ngx_event_t                        ngx_cache_purge_event;
static ngx_http_cache_purge_main_conf_t  *ngx_cache_purge_main_conf;


/* -- main configuration ------------------------------------------------- */

static void *
ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_cache_purge_main_conf_t *cmcf;

    cmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_main_conf_t));
    if (cmcf == NULL) {
        return NULL;
    }

    cmcf->background_purge    = NGX_CONF_UNSET;
    cmcf->queue_size          = NGX_CONF_UNSET_UINT;
    cmcf->batch_size          = NGX_CONF_UNSET_UINT;
    cmcf->throttle_ms         = NGX_CONF_UNSET_MSEC;
    cmcf->legacy_status_codes = NGX_CONF_UNSET;
    cmcf->vary_aware          = NGX_CONF_UNSET;

    return cmcf;
}

static char *
ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf = conf;
    ngx_str_t                         name = ngx_string("cache_purge_queue");
    size_t                            shm_size;
    size_t                            stride;
    ngx_shm_zone_t                   *old_zone, *zones;
    ngx_list_part_t                  *part;
    ngx_uint_t                        i;

    ngx_conf_init_value(cmcf->background_purge,    0);
    ngx_conf_init_uint_value(cmcf->queue_size,     NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT);
    ngx_conf_init_uint_value(cmcf->batch_size,     NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT);
    ngx_conf_init_msec_value(cmcf->throttle_ms,    NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT);
    /* Default on: return 412 for missing entries (backwards compatibility) */
    ngx_conf_init_value(cmcf->legacy_status_codes, 1);
    /* Default off: vary-aware walk adds cost; opt in explicitly */
    ngx_conf_init_value(cmcf->vary_aware,          0);

    /* Reject zero queue and batch limits. */
    if (cmcf->queue_size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_queue_size must be greater than 0");
        return NGX_CONF_ERROR;
    }

    if (cmcf->batch_size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_batch_size must be greater than 0");
        return NGX_CONF_ERROR;
    }

    old_zone = NULL;
    for (part = &cf->cycle->old_cycle->shared_memory.part;
         part != NULL; part = part->next)
    {
        zones = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (zones[i].tag == &ngx_http_cache_purge_module) {
                old_zone = &zones[i];
                break;
            }
        }
    }

    if (!cmcf->background_purge && old_zone == NULL) {
        return NGX_CONF_OK;
    }

    /*
     * Guard against unsigned integer overflow in the shm_size calculation.
     *
     *   shm_size = sizeof(queue_t)
     *            + queue_size * sizeof(item_t)
     *            + queue_size * 2 * KEY_MAX_LEN
     *            = sizeof(queue_t) + queue_size * stride
     *
     * Overflow condition (unsigned arithmetic):
     *   queue_size > (SIZE_MAX - sizeof(queue_t)) / stride
     *
     * where SIZE_MAX is represented as (size_t) -1, valid in C89/C90.
     */
    stride = sizeof(ngx_http_cache_purge_queue_item_t)
             + 2 * NGX_CACHE_PURGE_KEY_MAX_LEN;

    if (cmcf->queue_size > ((size_t) -1
                            - sizeof(ngx_http_cache_purge_queue_t)
                            - (ngx_pagesize - 1)) / stride)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "cache_purge_queue_size %ui overflows shared "
                           "memory size calculation", cmcf->queue_size);
        return NGX_CONF_ERROR;
    }

    shm_size = sizeof(ngx_http_cache_purge_queue_t)
             + cmcf->queue_size * sizeof(ngx_http_cache_purge_queue_item_t)
             + cmcf->queue_size * 2 * NGX_CACHE_PURGE_KEY_MAX_LEN;

    /*
     * The slab allocator imposes metadata overhead that is NOT reflected in
     * the payload calculation above:
     *
     *   ngx_slab_pool_t header  -- size varies by nginx version, build flags
     *                             (e.g. NGX_HAVE_POSIX_SEM, --with-threads),
     *                             and platform ABI.
     *   Slot descriptors        -- (pagesize_shift - min_shift) page structs.
     *   Stat entries            -- same count, added in nginx ~1.9.x.
     *   Page descriptors        -- one per allocatable page.
     *   Alignment gap           -- up to one full page between descriptors
     *                             and the first usable byte (pool->start is
     *                             rounded up to the next page boundary).
     *
     * Attempting to compute this precisely requires knowledge of internal
     * nginx structs that differ across versions (1.8 vs 1.9+), build
     * configurations, and architectures (Linux, *BSD, Solaris, macOS --
     * each may have different page sizes: 4 KB, 8 KB, 16 KB, 64 KB).
     *
     * The portable, version-agnostic approach: round up the payload to a
     * page boundary, then enforce a minimum of NGX_CACHE_PURGE_SHM_MIN_PAGES
     * pages.  This minimum is chosen so that, on every supported platform,
     * the slab overhead leaves at least one full page of usable heap even
     * when queue_size=1.  ngx_pagesize is always the runtime system page
     * size, so the minimum scales automatically on big-page architectures.
     */
    shm_size = ngx_align(shm_size, ngx_pagesize);

    if (shm_size < NGX_CACHE_PURGE_SHM_MIN_PAGES * ngx_pagesize) {
        shm_size = NGX_CACHE_PURGE_SHM_MIN_PAGES * ngx_pagesize;
    }

    if (old_zone != NULL) {
        if (cmcf->background_purge && shm_size != old_zone->shm.size) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                              "cache purge queue zone resizing requires "
                              "a restart");
            return NGX_CONF_ERROR;
        }

        /* Keep accepted work reachable when enqueueing is disabled. */
        shm_size = old_zone->shm.size;
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

/* Reuse the queue and bind status locations before workers are forked. */
static ngx_int_t
ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_cache_purge_main_conf_t  *cmcf = shm_zone->data;
    ngx_http_cache_purge_main_conf_t  *old = data;
    ngx_http_cache_purge_queue_t      *queue;
    ngx_http_cache_purge_status_t     *status;
    ngx_slab_pool_t                  *shpool;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (old != NULL) {
        queue = old->queue;

    } else if (shm_zone->shm.exists) {
        queue = shpool->data;

    } else {
        queue = ngx_slab_calloc(shpool, sizeof(*queue));
        if (queue == NULL) {
            return NGX_ERROR;
        }

        queue->shpool = shpool;
        shpool->data = queue;
    }

    cmcf->queue = queue;
    ngx_shmtx_lock(&shpool->mutex);

    for (status = cmcf->statuses; status != NULL; status = status->next) {
        status->path = ngx_http_cache_purge_path(queue, &status->name);
        if (status->path == NULL) {
            ngx_shmtx_unlock(&shpool->mutex);
            return NGX_ERROR;
        }
    }

    if (cmcf->background_purge) {
        queue->max_size = cmcf->queue_size;
    }

    ngx_shmtx_unlock(&shpool->mutex);

    return NGX_OK;
}


/* -- worker lifecycle --------------------------------------------------- */

static ngx_int_t
ngx_http_cache_purge_init_worker(ngx_cycle_t *cycle)
{
    ngx_http_core_main_conf_t        *cmcf_core;
    ngx_http_cache_purge_main_conf_t *cmcf;

    cmcf_core = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_core_module);
    if (cmcf_core == NULL) {
        return NGX_OK;
    }

    cmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_cache_purge_module);
    if (cmcf == NULL || cmcf->queue == NULL) {
        return NGX_OK;
    }

    ngx_cache_purge_main_conf = cmcf;

    ngx_memzero(&ngx_cache_purge_event, sizeof(ngx_event_t));
    ngx_cache_purge_event.handler     = ngx_http_cache_purge_background_handler;
    ngx_cache_purge_event.log         = cycle->log;
    ngx_cache_purge_event.data        = cycle;
    /* Do not keep an exiting worker alive solely for this timer. */
    ngx_cache_purge_event.cancelable  = 1;

    ngx_add_timer(&ngx_cache_purge_event, cmcf->throttle_ms);

    return NGX_OK;
}

static void
ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle)
{
    (void) cycle;

    if (ngx_cache_purge_event.timer_set) {
        ngx_del_timer(&ngx_cache_purge_event);
    }
}

/*
 * Background timer callback -- fires every throttle_ms milliseconds.
 *
 * Each invocation calls process_queue(), which dequeues and walks exactly
 * one item before returning.  This one-item-per-tick design ensures the
 * event loop is never blocked for more than the duration of a single
 * directory walk, regardless of queue depth.
 *
 *   NGX_AGAIN  -- one item was processed; re-arm with throttle_ms so the
 *                next item is handled promptly.
 *
 *   NGX_OK     -- queue is empty; re-arm with throttle_ms * 10 to avoid
 *                busy-polling on an idle queue.
 *
 *   NGX_ERROR  -- module not yet initialised; use the raw constant and
 *                retry next tick.
 *
 * Historical note: the previous implementation called ngx_msleep() inside
 * the callback to throttle I/O.  ngx_msleep() is a literal usleep() that
 * blocks the OS thread -- stalling every connection on that worker for the
 * full sleep duration.  Timer-based yielding is the correct nginx idiom.
 */
static void
ngx_http_cache_purge_background_handler(ngx_event_t *ev)
{
    ngx_cycle_t                      *cycle = ev->data;
    ngx_http_cache_purge_main_conf_t *cmcf  = ngx_cache_purge_main_conf;
    ngx_int_t                         rc;
    ngx_msec_t                        next_delay;

    if (ngx_terminate || ngx_quit || ngx_exiting) {
        return;
    }

    if (cmcf == NULL || cmcf->queue == NULL) {
        /* cmcf not yet initialised; use the raw-ms constant directly
         * (not through ngx_parse_time, so no *1000 conversion). */
        ngx_add_timer(ev, NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT);
        return;
    }

    rc = ngx_http_cache_purge_process_queue(cycle);

    /*
     * NGX_AGAIN  -> an item was processed and more remain; come back soon.
     * NGX_OK     -> queue is now empty; back off to avoid busy-polling.
     * NGX_ERROR  -> configuration problem; back off.
     */
    next_delay = (rc == NGX_AGAIN) ? cmcf->throttle_ms
                                   : cmcf->throttle_ms * 10;

    ngx_add_timer(ev, next_delay);
}


/* -- queue operations --------------------------------------------------- */

static ngx_int_t
ngx_http_cache_purge_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all)
{
    ngx_http_cache_purge_main_conf_t   *cmcf;
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *item;
    ngx_http_cache_purge_path_t        *path;
    ngx_uint_t                          hash, size, max;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    if (cmcf == NULL || cmcf->queue == NULL || !cmcf->background_purge) {
        return NGX_ERROR;
    }

    if (key->len > (size_t) -1 - sizeof(*item) - 1) {
        return NGX_ERROR;
    }

    queue = cmcf->queue;
    hash = ngx_http_cache_purge_hash_key(&cache->path->name, key);

    ngx_shmtx_lock(&queue->shpool->mutex);

    if (ngx_http_cache_purge_find_duplicate(queue, hash,
                                          &cache->path->name, key, purge_all)
        != NULL)
    {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_OK;
    }

    if (queue->size >= queue->max_size) {
        size = queue->size;
        max = queue->max_size;
        ngx_shmtx_unlock(&queue->shpool->mutex);
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_cache_purge: queue full (%ui/%ui items), "
                      "falling back to synchronous purge", size, max);
        return NGX_ERROR;
    }

    path = ngx_http_cache_purge_path(queue, &cache->path->name);
    if (path == NULL) {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_ERROR;
    }

    item = ngx_slab_alloc_locked(queue->shpool, sizeof(*item) + key->len + 1);
    if (item == NULL) {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_ERROR;
    }

    item->path = path;
    item->cache_path = path->name;
    item->key_partial.len = key->len;
    item->key_partial.data = (u_char *) (item + 1);
    if (key->len != 0) {
        ngx_memcpy(item->key_partial.data, key->data, key->len);
    }
    item->key_partial.data[key->len] = '\0';
    item->hash = hash;
    item->purge_all = purge_all;
    item->enqueued_at = ngx_current_msec;
    item->next = NULL;

    if (queue->tail != NULL) {
        queue->tail->next = item;
    } else {
        queue->head = item;
    }
    queue->tail = item;
    queue->size++;
    path->waiting++;
    path->purge_all += (purge_all != 0);

    ngx_shmtx_unlock(&queue->shpool->mutex);

    return NGX_OK;
}

/*
 * Dequeue and publish activity atomically; walk and free outside the mutex.
 * Process one item, including an expired item, per timer invocation.
 */
static ngx_int_t
ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle)
{
    ngx_http_cache_purge_main_conf_t   *cmcf;
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *item;
    ngx_http_cache_purge_walk_ctx_t     ctx;
    ngx_tree_ctx_t                      tree;
    ngx_msec_int_t                      age;

    cmcf = ngx_cache_purge_main_conf;
    if (cmcf == NULL || cmcf->queue == NULL) {
        return NGX_ERROR;
    }

    queue = cmcf->queue;
    ngx_shmtx_lock(&queue->shpool->mutex);
    item = queue->head;
    if (item == NULL) {
        ngx_shmtx_unlock(&queue->shpool->mutex);
        return NGX_OK;
    }

    queue->head = item->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    queue->size--;
    item->path->waiting--;
    item->path->purge_all -= (item->purge_all != 0);
    item->path->active++;
    ngx_shmtx_unlock(&queue->shpool->mutex);

    /* Signed modular delta tolerates clock skew and millisecond rollover. */
    age = (ngx_msec_int_t) (ngx_current_msec - item->enqueued_at);
    if (age > NGX_CACHE_PURGE_QUEUE_TIMEOUT) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "ngx_cache_purge: purge of \"%V\" key \"%V\" "
                      "timed out, discarding",
                      &item->cache_path, &item->key_partial);
        goto done;
    }

    ngx_memzero(&ctx, sizeof(ctx));
    ngx_memzero(&tree, sizeof(tree));
    ctx.root_len = item->cache_path.len;
    tree.pre_tree_handler = ngx_http_cache_purge_pre_tree;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler = ngx_http_purge_file_cache_noop;
    tree.data = &ctx;
    tree.log = cycle->log;

    if (item->purge_all) {
        tree.file_handler = ngx_http_purge_file_cache_delete_file;

    } else {
        ctx.key_partial = item->key_partial.data;
        ctx.key_len = item->key_partial.len;
        if (ctx.key_len != 0 && ctx.key_partial[ctx.key_len - 1] == '*') {
            ctx.key_len--;
        }
        tree.file_handler = ngx_http_purge_file_cache_delete_partial_file;
    }

    (void) ngx_walk_tree(&tree, &item->cache_path);

done:

    ngx_shmtx_lock(&queue->shpool->mutex);
    item->path->active--;
    ngx_shmtx_unlock(&queue->shpool->mutex);
    ngx_slab_free(queue->shpool, item);

    return NGX_AGAIN;
}

static ngx_uint_t
ngx_http_cache_purge_hash_key(ngx_str_t *cache_path, ngx_str_t *key)
{
    ngx_uint_t  hash = 0;
    ngx_uint_t  i;

    for (i = 0; i < cache_path->len; i++) {
        hash = hash * 31 + cache_path->data[i];
    }
    for (i = 0; i < key->len; i++) {
        hash = hash * 31 + key->data[i];
    }

    return hash;
}

static ngx_http_cache_purge_queue_item_t *
ngx_http_cache_purge_find_duplicate(ngx_http_cache_purge_queue_t *queue,
    ngx_uint_t hash, ngx_str_t *cache_path, ngx_str_t *key,
    ngx_flag_t purge_all)
{
    ngx_http_cache_purge_queue_item_t *item;

    for (item = queue->head; item; item = item->next) {
        /*
         * Two-step check: hash first (fast), then full string comparison.
         * Comparing only the hash is insufficient because a 32-bit
         * multiplier hash will collide for distinct keys in large caches,
         * causing legitimate purge requests to be silently discarded.
         */
        if (item->hash != hash || item->purge_all != purge_all) {
            continue;
        }

        if (item->cache_path.len != cache_path->len
            || ngx_memcmp(item->cache_path.data, cache_path->data,
                          cache_path->len) != 0)
        {
            continue;
        }

        if (item->key_partial.len != key->len) {
            continue;
        }

        if (key->len > 0
            && ngx_memcmp(item->key_partial.data, key->data, key->len) != 0)
        {
            continue;
        }

        return item;
    }

    return NULL;
}


/* -- directive callbacks ------------------------------------------------ */

char *
ngx_http_cache_purge_queue_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf  = conf;
    ngx_str_t                        *value = cf->args->elts;

    (void) cmd;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        cmcf->background_purge = 1;
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        cmcf->background_purge = 0;
    } else {
        return "invalid value, use 'on' or 'off'";
    }

    return NGX_CONF_OK;
}

char *
ngx_http_cache_purge_legacy_status_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf  = conf;
    ngx_str_t                        *value = cf->args->elts;

    (void) cmd;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        cmcf->legacy_status_codes = 1;  /* 412 Precondition Failed */
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        cmcf->legacy_status_codes = 0;  /* 404 Not Found           */
    } else {
        return "invalid value, use 'on' or 'off'";
    }

    return NGX_CONF_OK;
}

char *
ngx_http_cache_purge_vary_aware_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_main_conf_t *cmcf  = conf;
    ngx_str_t                        *value = cf->args->elts;

    (void) cmd;

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        cmcf->vary_aware = 1;
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        cmcf->vary_aware = 0;
    } else {
        return "invalid value, use 'on' or 'off'";
    }

    return NGX_CONF_OK;
}


/* Caller holds shpool->mutex.  Names remain valid until the zone is freed. */
static ngx_http_cache_purge_path_t *
ngx_http_cache_purge_path(ngx_http_cache_purge_queue_t *queue, ngx_str_t *name)
{
    ngx_http_cache_purge_path_t  *path;

    for (path = queue->paths; path != NULL; path = path->next) {
        if (path->name.len == name->len
            && ngx_filename_cmp(path->name.data, name->data, name->len) == 0)
        {
            return path;
        }
    }

    if (name->len > (size_t) -1 - sizeof(*path) - 1) {
        return NULL;
    }

    path = ngx_slab_calloc_locked(queue->shpool,
                                 sizeof(*path) + name->len + 1);
    if (path == NULL) {
        return NULL;
    }

    path->name.len = name->len;
    path->name.data = (u_char *) (path + 1);
    ngx_memcpy(path->name.data, name->data, name->len);
    path->next = queue->paths;
    queue->paths = path;

    return path;
}


static char *
ngx_http_cache_purge_status_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_cache_purge_loc_conf_t   *cplcf = conf;
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_status_t     *status;
    ngx_http_core_loc_conf_t          *clcf;
    ngx_str_t                         *value;

    (void) cmd;

    if (cplcf->status != NULL) {
        return "is duplicate";
    }

    value = cf->args->elts;
    if (value[1].len == 0) {
        return "path must not be empty";
    }

    status = ngx_pcalloc(cf->pool, sizeof(*status));
    if (status == NULL) {
        return NGX_CONF_ERROR;
    }

    status->name = value[1];
    if (status->name.len > 1
        && status->name.data[status->name.len - 1] == '/')
    {
        status->name.len--;
    }

    if (ngx_conf_full_name(cf->cycle, &status->name, 0) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);
    status->next = cmcf->statuses;
    cmcf->statuses = status;
    cplcf->status = status;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_cache_purge_status_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_cache_purge_status_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_loc_conf_t   *cplcf;
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_path_t       *path;
    ngx_uint_t                         waiting, all, full, active;
    ngx_table_elt_t                   *cc;
    ngx_buf_t                         *b;
    ngx_chain_t                        out;
    ngx_int_t                          rc;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    if (cmcf->queue == NULL || cplcf->status == NULL
        || cplcf->status->path == NULL)
    {
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    path = cplcf->status->path;

    ngx_shmtx_lock(&cmcf->queue->shpool->mutex);
    waiting = path->waiting;
    all = path->purge_all;
    active = path->active;
    full = (cmcf->queue->size >= cmcf->queue->max_size);
    ngx_shmtx_unlock(&cmcf->queue->shpool->mutex);

    /* At most 104 bytes on 64-bit targets; no NUL terminator is needed. */
    b = ngx_create_temp_buf(r->pool, 160);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cc = ngx_list_push(&r->headers_out.headers);
    if (cc == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_memzero(cc, sizeof(*cc));
    cc->hash = 1;
    ngx_str_set(&cc->key, "Cache-Control");
    ngx_str_set(&cc->value, "no-store");

    b->last = ngx_snprintf(b->last, b->end - b->last,
                          "{\"queue_size\":%ui,\"purge_all_pending\":%s,"
                          "\"queue_full\":%s,\"purge_in_flight\":%s}",
                          waiting, all ? "true" : "false",
                          full ? "true" : "false",
                          active ? "true" : "false");
    if (b->last == b->end) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->last_buf = (r == r->main);
    b->last_in_chain = 1;
    out.buf = b;
    out.next = NULL;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;
    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_type_len = r->headers_out.content_type.len;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


#if (NGX_WIN32)
#define ngx_http_cache_purge_separator(c)  ((c) == '/' || (c) == '\\')
#else
#define ngx_http_cache_purge_separator(c)  ((c) == '/')
#endif


static ngx_uint_t
ngx_http_cache_purge_protected(ngx_str_t *path, size_t root_len)
{
    size_t            start, end, n;
    ngx_uint_t        i;
    static ngx_str_t  names[] = {
        ngx_string("client_temp"), ngx_string("client_body_temp"),
        ngx_string("fastcgi_temp"), ngx_string("proxy_temp"),
        ngx_string("scgi_temp"), ngx_string("uwsgi_temp")
    };

    /* Only complete components below the walk root are inspected. */
    for (start = root_len; start < path->len; start = end + 1) {
        for (end = start; end < path->len; end++) {
            if (ngx_http_cache_purge_separator(path->data[end])) {
                break;
            }
        }

        n = end - start;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (n == names[i].len
                && ngx_filename_cmp(path->data + start, names[i].data, n)
                   == 0)
            {
                return 1;
            }
        }

        if (end == path->len) {
            break;
        }
    }

    return 0;
}


static ngx_int_t
ngx_http_cache_purge_pre_tree(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    ngx_http_cache_purge_walk_ctx_t  *wctx = ctx->data;

    return ngx_http_cache_purge_protected(path, wctx->root_len)
           ? NGX_DECLINED : NGX_OK;
}


static ngx_uint_t
ngx_http_cache_purge_cache_file(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    ngx_http_cache_purge_walk_ctx_t  *wctx = ctx->data;
    u_char                          *p, c;
    ngx_uint_t                       i;

    if (path->len <= wctx->root_len || path->len - wctx->root_len < 33
        || ngx_http_cache_purge_protected(path, wctx->root_len))
    {
        return 0;
    }

    /* Cache files have 32 hex digits; temp files have 10 digits or a suffix. */
    p = path->data + path->len - 32;
    if (!ngx_http_cache_purge_separator(p[-1])) {
        return 0;
    }

    for (i = 0; i < 32; i++) {
        c = ngx_tolower(p[i]);
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) {
            return 0;
        }
    }

    return 1;
}


/* -- file-walk helpers -------------------------------------------------- */

/*
 * Prefix-match walk handler.
 *
 * Uses walk_ctx->key_buffer (stack-allocated, fixed size) instead of
 * allocating from ngx_cycle pool on every file visit.  This eliminates
 * the memory leak that occurred on partial purges over large caches.
 */
static ngx_int_t
ngx_http_purge_file_cache_delete_partial_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path)
{
    ngx_http_cache_purge_walk_ctx_t *wctx;
    ngx_file_t                       file;
    ngx_flag_t                       remove_file = 0;
    ngx_int_t                        n;

    wctx = ctx->data;
    wctx->files_checked++;

    if (!ngx_http_cache_purge_cache_file(ctx, path)) {
        return NGX_OK;
    }

    if (wctx->key_len == 0) {
        /* stripped wildcard -- match everything */
        remove_file = 1;

    } else {
        if (wctx->key_len >= NGX_CACHE_PURGE_KEY_MAX_LEN) {
            ngx_log_error(NGX_LOG_WARN, ctx->log, 0,
                          "ngx_cache_purge: key too long (%uz bytes), "
                          "skipping \"%V\"", wctx->key_len, path);
            return NGX_OK;
        }

        ngx_memzero(&file, sizeof(ngx_file_t));
        file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
        if (file.fd == NGX_INVALID_FILE) {
            return NGX_OK;
        }
        file.log = ctx->log;

        /*
         * Cache file layout:
         *   ngx_http_file_cache_header_t | "\nKEY: " | <key> | "\n"
         * Skip the 6-byte "\nKEY: " prefix.
         */
        n = ngx_read_file(&file, wctx->key_buffer, wctx->key_len,
                          sizeof(ngx_http_file_cache_header_t) + NGX_CACHE_PURGE_KEY_HDR_OFFSET);
        ngx_close_file(file.fd);

        if (n == (ngx_int_t) wctx->key_len
            && ngx_strncasecmp(wctx->key_buffer, wctx->key_partial,
                               wctx->key_len) == 0)
        {
            remove_file = 1;
        }
    }

    if (remove_file) {
        if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                          "ngx_cache_purge: could not delete \"%V\"", path);
        } else {
            wctx->files_deleted++;
        }
    }

    return NGX_OK;
}

/*
 * ngx_http_cache_purge_invalidate_node -- clear a cache node's shared-memory
 * metadata without requiring an ngx_http_cache_t context.
 *
 * The nginx file cache rbtree uses the FULL 16-byte MD5 key for lookup:
 *
 *   node->key (ngx_rbtree_key_t, 4 bytes) = first 4 bytes of key as big-endian
 *   fcn->key  (u_char[12])                = remaining 12 bytes of MD5
 *
 * The rbtree insert function (ngx_http_file_cache_rbtree_insert_value) resolves
 * collisions on the 4-byte prefix by falling back to a memcmp of the trailing
 * 12 bytes and placing the new node left or right accordingly.  This gives the
 * tree a total ordering on all 16 bytes, so a standard BST search works.
 *
 * The node's 16-byte key is encoded in the file path: nginx constructs the
 * cache file path as "<cache_root>/<levels>/<32-hex-char-key>" where the last
 * 32 characters are the hex-encoded 16-byte MD5 key.  Decoding those characters
 * gives us the exact key to search for -- correct for both primary entries and
 * Vary variant entries (which have different keys XOR'd with the secondary hash).
 *
 * Locking: shpool->mutex is held for the shortest possible window (lookup +
 * field updates only).  ngx_delete_file() is called by the caller AFTER this
 * function returns and the lock is released, preserving the module-wide lock
 * ordering: the queue zone's mutex is not held while walking cache files.
 */
static void
ngx_http_cache_purge_invalidate_node(ngx_http_file_cache_t *cache,
    ngx_str_t *path)
{
    ngx_rbtree_node_t          *node;
    ngx_rbtree_node_t          *sentinel;
    ngx_http_file_cache_node_t *fcn;
    ngx_rbtree_key_t            lookup4;
    u_char                      key16[NGX_HTTP_CACHE_KEY_LEN];
    u_char                     *p;
    ngx_uint_t                  i;
    u_int                       hi, lo;
    int                         cmp;

    /*
     * C89: all variables declared above.
     *
     * Extract the 16-byte cache key from the last 32 characters of the file
     * path.  The path always ends with a 32-character lowercase-hex filename.
     */
    if (path->len < 32) {
        return;
    }

    p = path->data + path->len - 32;

    for (i = 0; i < NGX_HTTP_CACHE_KEY_LEN; i++) {
        hi = (u_int) p[i * 2];
        lo = (u_int) p[i * 2 + 1];

        if      (hi >= '0' && hi <= '9') { hi -= '0'; }
        else if (hi >= 'a' && hi <= 'f') { hi -= (u_int)('a' - 10); }
        else if (hi >= 'A' && hi <= 'F') { hi -= (u_int)('A' - 10); }
        else                             { return; /* not a valid hex path */ }

        if      (lo >= '0' && lo <= '9') { lo -= '0'; }
        else if (lo >= 'a' && lo <= 'f') { lo -= (u_int)('a' - 10); }
        else if (lo >= 'A' && lo <= 'F') { lo -= (u_int)('A' - 10); }
        else                             { return; }

        key16[i] = (u_char) ((hi << 4) | lo);
    }

    /*
     * Big-endian reconstruction of the 4-byte rbtree key from the first
     * 4 bytes of the 16-byte MD5.  Mirrors nginx's own key construction in
     * ngx_http_file_cache_set_header().
     */
    lookup4 = ((ngx_rbtree_key_t) key16[0] << 24)
            | ((ngx_rbtree_key_t) key16[1] << 16)
            | ((ngx_rbtree_key_t) key16[2] <<  8)
            |  (ngx_rbtree_key_t) key16[3];

    ngx_shmtx_lock(&cache->shpool->mutex);

    node     = cache->sh->rbtree.root;
    sentinel = cache->sh->rbtree.sentinel;

    while (node != sentinel) {

        if (lookup4 < node->key) {
            node = node->left;
            continue;
        }

        if (lookup4 > node->key) {
            node = node->right;
            continue;
        }

        /*
         * 4-byte prefix matches.  Compare the remaining 12 bytes
         * (key16[4..15] vs fcn->key[0..11]) using the same memcmp ordering
         * that ngx_http_file_cache_rbtree_insert_value uses.
         */
        fcn = (ngx_http_file_cache_node_t *) node;
        cmp = ngx_memcmp(key16 + sizeof(ngx_rbtree_key_t),
                         fcn->key,
                         NGX_HTTP_CACHE_KEY_LEN - sizeof(ngx_rbtree_key_t));

        if (cmp < 0) {
            node = node->left;
            continue;
        }

        if (cmp > 0) {
            node = node->right;
            continue;
        }

        /* Exact 16-byte match -- clear the node's accounting fields */
        if (fcn->exists) {
#if (nginx_version >= 1000001)
            cache->sh->size -= fcn->fs_size;
            fcn->fs_size     = 0;
#else
            cache->sh->size -= (fcn->length + cache->bsize - 1) / cache->bsize;
            fcn->length       = 0;
#endif
            fcn->exists = 0;
        }

        break;
    }

    ngx_shmtx_unlock(&cache->shpool->mutex);
}

/*
 * Exact-match walk handler (vary-aware).
 *
 * Reads (key_len + 1) bytes from the KEY: region of each cache file.
 * The file is deleted only when:
 *   - the first key_len bytes match key_partial exactly (case-insensitive), AND
 *   - the byte at position key_len is '\n' (exact-length confirmation)
 *
 * The '\n' check prevents false matches against keys that share a common prefix.
 * Because all Vary variants of a cached response store the same KEY: string,
 * this walk removes every variant file regardless of its filesystem path.
 *
 * The primary file was already deleted by ngx_http_file_cache_purge() which
 * correctly updated its rbtree node.  For each VARIANT file this handler finds,
 * it calls ngx_http_cache_purge_invalidate_node() to clear the variant's own
 * rbtree node metadata BEFORE calling ngx_delete_file(), so that:
 *   - cache->sh->size remains accurate (no phantom disk-space accounting)
 *   - the node's exists flag is cleared (no stale HIT responses)
 *
 * If the primary file appears again during the walk (its node was already
 * cleared), ngx_delete_file() returns ENOENT which is silently ignored.
 * invalidate_node() on an already-cleared node is a no-op (fcn->exists == 0).
 */
static ngx_int_t
ngx_http_purge_file_cache_delete_exact_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path)
{
    ngx_http_cache_purge_walk_ctx_t *wctx;
    ngx_file_t                       file;
    ngx_int_t                        n;

    wctx = ctx->data;
    wctx->files_checked++;

    if (!ngx_http_cache_purge_cache_file(ctx, path)) {
        return NGX_OK;
    }

    /* key_len == 0 or buffer too small to hold key + '\n' terminator: skip */
    if (wctx->key_len == 0
        || wctx->key_len + 1 >= NGX_CACHE_PURGE_KEY_MAX_LEN)
    {
        return NGX_OK;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (file.fd == NGX_INVALID_FILE) {
        return NGX_OK;
    }
    file.log = ctx->log;

    /* Read key_len + 1 bytes: the key string followed by its '\n' terminator */
    n = ngx_read_file(&file, wctx->key_buffer, wctx->key_len + 1,
                      sizeof(ngx_http_file_cache_header_t)
                      + NGX_CACHE_PURGE_KEY_HDR_OFFSET);
    ngx_close_file(file.fd);

    if (n != (ngx_int_t)(wctx->key_len + 1)) {
        return NGX_OK;
    }

    /* Exact-length check: the next byte must be '\n' */
    if (wctx->key_buffer[wctx->key_len] != '\n') {
        return NGX_OK;
    }

    if (ngx_strncasecmp(wctx->key_buffer, wctx->key_partial,
                        wctx->key_len) != 0)
    {
        return NGX_OK;
    }

    /*
     * Key confirmed.  Update the rbtree node's shared-memory metadata
     * BEFORE deleting the file.  This keeps cache->sh->size accurate and
     * prevents subsequent requests from getting a stale HIT on a missing file.
     */
    if (wctx->cache != NULL) {
        ngx_http_cache_purge_invalidate_node(wctx->cache, path);
    }

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        /* ENOENT: primary file was already deleted -- not an error */
        if (ngx_errno != NGX_ENOENT) {
            ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                          "ngx_cache_purge: could not delete \"%V\"", path);
        }
    } else {
        wctx->files_deleted++;
    }

    return NGX_OK;
}

/*
 * Walk the cache directory and delete all files whose KEY: string matches
 * the purged key exactly.  Called after a successful ngx_http_file_cache_purge()
 * when cache_purge_vary_aware is on.
 */
static void
ngx_http_cache_purge_delete_variants(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_walk_ctx_t  ctx;
    ngx_tree_ctx_t                   tree;
    ngx_str_t                       *key;

    key = r->cache->keys.elts;

    if (key[0].len == 0) {
        return;
    }

    ngx_memzero(&ctx,  sizeof(ngx_http_cache_purge_walk_ctx_t));
    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));

    ctx.key_partial = key[0].data;
    ctx.key_len     = key[0].len;
    ctx.root_len    = cache->path->name.len;
    ctx.cache       = cache;   /* enables shm metadata updates in the walk */

    tree.file_handler      = ngx_http_purge_file_cache_delete_exact_file;
    tree.pre_tree_handler  = ngx_http_cache_purge_pre_tree;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler      = ngx_http_purge_file_cache_noop;
    tree.data              = &ctx;
    tree.log               = ngx_cycle->log;

    ngx_walk_tree(&tree, &cache->path->name);

    if (ctx.files_deleted > 0) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: vary-aware walk deleted %ui variant(s) "
                       "for key \"%V\"", ctx.files_deleted, &key[0]);
    }
}

static ngx_int_t
ngx_http_purge_file_cache_noop(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    (void) ctx;
    (void) path;
    return NGX_OK;
}

static ngx_int_t
ngx_http_purge_file_cache_delete_file(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    ngx_http_cache_purge_walk_ctx_t  *wctx = ctx->data;

    if (!ngx_http_cache_purge_cache_file(ctx, path)) {
        return NGX_OK;
    }

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        if (ngx_errno != NGX_ENOENT) {
            ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                          "ngx_cache_purge: could not delete \"%V\"", path);
        }

    } else {
        wctx->files_deleted++;
    }

    return NGX_OK;
}


/* -- FastCGI ------------------------------------------------------------ */

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
    ngx_http_upstream_conf_t   upstream;
    ngx_str_t                  index;

#  if (nginx_version >= 1007008)
    ngx_http_fastcgi_params_t  params;
    ngx_http_fastcgi_params_t  params_cache;
#  else
    ngx_array_t               *flushes;
    ngx_array_t               *params_len;
    ngx_array_t               *params;
#  endif

    ngx_array_t               *params_source;
    ngx_array_t               *catch_stderr;
    ngx_array_t               *fastcgi_lengths;
    ngx_array_t               *fastcgi_values;

#  if (nginx_version >= 8040) && (nginx_version < 1007008)
    ngx_hash_t                 headers_hash;
    ngx_uint_t                 header_params;
#  endif

#  if (nginx_version >= 1001004)
    ngx_flag_t                 keep_conn;
#  endif

    ngx_http_complex_value_t   cache_key;

#  if (NGX_PCRE)
    ngx_regex_t               *split_regex;
    ngx_str_t                  split_name;
#  endif
} ngx_http_fastcgi_loc_conf_t;

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
    ngx_http_complex_value_t          cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->fastcgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
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
#  endif
    {
        return "is incompatible with \"fastcgi_cache\"";
    }

    if (flcf->upstream.upstream || flcf->fastcgi_lengths) {
        return "is incompatible with \"fastcgi_pass\"";
    }

    if (flcf->upstream.store > 0
#  if (nginx_version < 1007009)
        || flcf->upstream.store_lengths
#  endif
       )
    {
        return "is incompatible with \"fastcgi_store\"";
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

ngx_int_t
ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_fastcgi_loc_conf_t      *flcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_fastcgi_main_conf_t     *fmcf;
    ngx_int_t                         rc;
#  endif

    ngx_str_set(&status, "purged");

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
#  endif

    if (ngx_http_cache_purge_init(r, cache, &flcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_enqueue(r, cache, &key[0],
                                         cplcf->conf->purge_all) == NGX_OK)
        {
            r->headers_out.status = NGX_HTTP_ACCEPTED;
            ngx_str_set(&status, "queued");
            r->main->count++;
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        /* purge_all empties the zone -- always report 200 regardless of
         * how many files existed.  Skip ngx_http_cache_purge_handler()
         * so we never attempt an exact-key lookup on a bulk operation. */
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        /* Return 200 only when at least one matching file was deleted.
         * On a complete miss return 412/404 per cache_purge_legacy_status. */
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_FASTCGI */


/* -- Proxy -------------------------------------------------------------- */

# if (NGX_HTTP_PROXY)
extern ngx_module_t  ngx_http_proxy_module;

#  if (nginx_version < 1029004)

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
    ngx_http_upstream_conf_t   upstream;

#  if (nginx_version >= 1007008)
    ngx_array_t               *body_flushes;
    ngx_array_t               *body_lengths;
    ngx_array_t               *body_values;
    ngx_str_t                  body_source;
    ngx_http_proxy_headers_t   headers;
    ngx_http_proxy_headers_t   headers_cache;
#  else
    ngx_array_t               *flushes;
    ngx_array_t               *body_set_len;
    ngx_array_t               *body_set;
    ngx_array_t               *headers_set_len;
    ngx_array_t               *headers_set;
    ngx_hash_t                 headers_set_hash;
#  endif

    ngx_array_t               *headers_source;
    /* FIX (#52): nginx 1.29.4 inserted host_set here -- without this guard
     * every subsequent field is at the wrong offset, causing a segfault. */
#  if (nginx_version >= 1029004)
    ngx_uint_t                 host_set;
#  endif
#  if (nginx_version < 8040)
    ngx_array_t               *headers_names;
#  endif

    ngx_array_t               *proxy_lengths;
    ngx_array_t               *proxy_values;
    ngx_array_t               *redirects;

#  if (nginx_version >= 1001015)
    ngx_array_t               *cookie_domains;
    ngx_array_t               *cookie_paths;
#  endif
#  if (nginx_version >= 1019003)
    ngx_array_t               *cookie_flags;
#  endif
#  if (nginx_version < 1007008)
    ngx_str_t                  body_source;
#  endif

#  if (nginx_version >= 1011006)
    ngx_http_complex_value_t  *method;
#  else
    ngx_str_t                  method;
#  endif
    ngx_str_t                  location;
    ngx_str_t                  url;

    ngx_http_complex_value_t   cache_key;
    ngx_http_proxy_vars_t      vars;
    ngx_flag_t                 redirect;

#  if (nginx_version >= 1001004)
    ngx_uint_t                 http_version;
#  endif

    ngx_uint_t                 headers_hash_max_size;
    ngx_uint_t                 headers_hash_bucket_size;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005006)
    ngx_uint_t                 ssl;
    ngx_uint_t                 ssl_protocols;
    ngx_str_t                  ssl_ciphers;
#    endif
#    if (nginx_version >= 1007000)
    ngx_uint_t                 ssl_verify_depth;
    ngx_str_t                  ssl_trusted_certificate;
    ngx_str_t                  ssl_crl;
#    endif
#    if (nginx_version >= 1007008) && (nginx_version < 1021000)
    ngx_str_t                  ssl_certificate;
    ngx_str_t                  ssl_certificate_key;
    ngx_array_t               *ssl_passwords;
#    endif
#    if (nginx_version >= 1019004)
    ngx_array_t               *ssl_conf_commands;
#    endif
#  endif
} ngx_http_proxy_loc_conf_t;

#  endif

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
    ngx_http_complex_value_t          cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->proxy.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
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
#  endif
    {
        return "is incompatible with \"proxy_cache\"";
    }

    if (plcf->upstream.upstream || plcf->proxy_lengths) {
        return "is incompatible with \"proxy_pass\"";
    }

    if (plcf->upstream.store > 0
#  if (nginx_version < 1007009)
        || plcf->upstream.store_lengths
#  endif
       )
    {
        return "is incompatible with \"proxy_store\"";
    }

    value = cf->args->elts;

    /*
     * FIX: do NOT set plcf->upstream.cache here.  In nginx >= 1.27 the proxy
     * module's merge_loc_conf walks every location that has upstream.cache set
     * and synthesises a default "location /" entry, which collides with the
     * explicit "location /" block in the user config and produces the fatal
     * "duplicate location" error at startup.
     *
     * Instead we store the zone reference and the purge-key template directly
     * in our own loc_conf fields (proxy_separate_zone / proxy_separate_value /
     * proxy_separate_key).  The handler below resolves them at request time
     * without going through plcf->upstream at all.
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

ngx_int_t
ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_proxy_loc_conf_t        *plcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_proxy_main_conf_t       *pmcf;
    ngx_int_t                         rc;
    ngx_uint_t                        i;        /* C89: declared at top of scope */
    ngx_str_t                        *name;     /* C89: declared at top of scope */
    ngx_http_file_cache_t           **caches;   /* C89: declared at top of scope */
    ngx_str_t                         cv_val;   /* C89: declared at top of scope */
#  endif

    ngx_str_set(&status, "purged");

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /*
     * Separate-location syntax (proxy_cache_purge zone key): the cache zone
     * and purge-key template are stored in cplcf, not in plcf->upstream.
     * Resolve them here without touching plcf->upstream.cache so that we
     * never trigger the nginx >= 1.27 duplicate-location synthesis.
     */
    if (cplcf->proxy.enable == 0
        && (cplcf->proxy_separate_zone || cplcf->proxy_separate_value))
    {
        if (cplcf->proxy_separate_zone) {
            cache = cplcf->proxy_separate_zone->data;
        } else {
            /* dynamic zone name -- evaluate and walk the proxy caches list */
            if (ngx_http_complex_value(r, cplcf->proxy_separate_value,
                                       &cv_val) != NGX_OK)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

#  if (nginx_version >= 1007009)
            pmcf   = ngx_http_get_module_main_conf(r, ngx_http_proxy_module);
            caches = pmcf->caches.elts;
            cache  = NULL;

            for (i = 0; i < pmcf->caches.nelts; i++) {
                name = &caches[i]->shm_zone->shm.name;
                if (name->len == cv_val.len
                    && ngx_strncmp(name->data, cv_val.data, cv_val.len) == 0)
                {
                    cache = caches[i];
                    break;
                }
            }

            if (cache == NULL) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "ngx_cache_purge: cache zone \"%V\" not found",
                              &cv_val);
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
#  else
            cache = cplcf->proxy_separate_zone->data;
#  endif
        }

        if (ngx_http_cache_purge_init(r, cache,
                                      &cplcf->proxy_separate_key) != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

    } else {
        /* Inline syntax (proxy_cache_purge METHOD from ...): use plcf->upstream
         * as before. */
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
#  endif

        if (ngx_http_cache_purge_init(r, cache, &plcf->cache_key) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_enqueue(r, cache, &key[0],
                                         cplcf->conf->purge_all) == NGX_OK)
        {
            r->headers_out.status = NGX_HTTP_ACCEPTED;
            ngx_str_set(&status, "queued");
            r->main->count++;
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_PROXY */


/* -- SCGI --------------------------------------------------------------- */

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
    ngx_http_scgi_params_t    params;
    ngx_http_scgi_params_t    params_cache;
    ngx_array_t              *params_source;
#  else
    ngx_array_t              *flushes;
    ngx_array_t              *params_len;
    ngx_array_t              *params;
    ngx_array_t              *params_source;
    ngx_hash_t                headers_hash;
    ngx_uint_t                header_params;
#  endif

    ngx_array_t              *scgi_lengths;
    ngx_array_t              *scgi_values;
    ngx_http_complex_value_t  cache_key;
} ngx_http_scgi_loc_conf_t;

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
    ngx_http_complex_value_t          cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->scgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
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
#  endif
    {
        return "is incompatible with \"scgi_cache\"";
    }

    if (slcf->upstream.upstream || slcf->scgi_lengths) {
        return "is incompatible with \"scgi_pass\"";
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

ngx_int_t
ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_scgi_loc_conf_t         *slcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_scgi_main_conf_t        *smcf;
    ngx_int_t                         rc;
#  endif

    ngx_str_set(&status, "purged");

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
#  endif

    if (ngx_http_cache_purge_init(r, cache, &slcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_enqueue(r, cache, &key[0],
                                         cplcf->conf->purge_all) == NGX_OK)
        {
            r->headers_out.status = NGX_HTTP_ACCEPTED;
            ngx_str_set(&status, "queued");
            r->main->count++;
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_SCGI */


/* -- uWSGI -------------------------------------------------------------- */

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
    ngx_http_uwsgi_params_t   params;
    ngx_http_uwsgi_params_t   params_cache;
    ngx_array_t              *params_source;
#  else
    ngx_array_t              *flushes;
    ngx_array_t              *params_len;
    ngx_array_t              *params;
    ngx_array_t              *params_source;
    ngx_hash_t                headers_hash;
    ngx_uint_t                header_params;
#  endif

    ngx_array_t              *uwsgi_lengths;
    ngx_array_t              *uwsgi_values;
    ngx_http_complex_value_t  cache_key;
    ngx_str_t                 uwsgi_string;
    ngx_uint_t                modifier1;
    ngx_uint_t                modifier2;

#  if (NGX_HTTP_SSL)
#    if (nginx_version >= 1005008)
    ngx_uint_t                ssl;
    ngx_uint_t                ssl_protocols;
    ngx_str_t                 ssl_ciphers;
#    endif
#    if (nginx_version >= 1007000)
    ngx_uint_t                ssl_verify_depth;
    ngx_str_t                 ssl_trusted_certificate;
    ngx_str_t                 ssl_crl;
#    endif
#    if (nginx_version >= 1007008) && (nginx_version < 1021000)
    ngx_str_t                 ssl_certificate;
    ngx_str_t                 ssl_certificate_key;
    ngx_array_t              *ssl_passwords;
#    endif
#    if (nginx_version >= 1019004)
    ngx_array_t              *ssl_conf_commands;
#    endif
#  endif
} ngx_http_uwsgi_loc_conf_t;

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
    ngx_http_complex_value_t          cv;
#  endif

    (void) cmd;
    (void) conf;

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->uwsgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
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
#  endif
    {
        return "is incompatible with \"uwsgi_cache\"";
    }

    if (ulcf->upstream.upstream || ulcf->uwsgi_lengths) {
        return "is incompatible with \"uwsgi_pass\"";
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

ngx_int_t
ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_file_cache_t            *cache;
    ngx_http_uwsgi_loc_conf_t        *ulcf;
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_str_t                        *key;
    ngx_uint_t                        deleted;  /* C89: declared at top of scope */
#  if (nginx_version >= 1007009)
    ngx_http_uwsgi_main_conf_t       *umcf;
    ngx_int_t                         rc;
#  endif

    ngx_str_set(&status, "purged");

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
#  endif

    if (ngx_http_cache_purge_init(r, cache, &ulcf->cache_key) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r,  ngx_http_cache_purge_module);

    if (cmcf != NULL && cmcf->background_purge
        && (cplcf->conf->purge_all || ngx_http_cache_purge_is_partial(r)))
    {
        key = r->cache->keys.elts;
        if (ngx_http_cache_purge_enqueue(r, cache, &key[0],
                                         cplcf->conf->purge_all) == NGX_OK)
        {
            r->headers_out.status = NGX_HTTP_ACCEPTED;
            ngx_str_set(&status, "queued");
            r->main->count++;
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
            return NGX_DONE;
        }
    }

    if (cplcf->conf->purge_all) {
        ngx_http_cache_purge_all(r, cache);
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        deleted = ngx_http_cache_purge_partial(r, cache);
        r->main->count++;
        if (deleted > 0) {
            ngx_http_finalize_request(r,
                ngx_http_cache_purge_send_response(r, &status));
        } else {
            ngx_http_finalize_request(r,
                (cmcf != NULL && cmcf->legacy_status_codes)
                         ? NGX_HTTP_PRECONDITION_FAILED
                         : NGX_HTTP_NOT_FOUND);
        }
        return NGX_DONE;
    }

    r->main->count++;

    ngx_http_cache_purge_handler(r);

    return NGX_DONE;
}
# endif /* NGX_HTTP_UWSGI */


/* -- status configuration ----------------------------------------------- */

static ngx_int_t
ngx_http_cache_purge_postconfiguration(ngx_conf_t *cf)
{
    ngx_http_cache_purge_main_conf_t  *cmcf;
    ngx_http_cache_purge_status_t     *status;
    ngx_http_file_cache_t             *cache;
    ngx_shm_zone_t                    *zones;
    ngx_list_part_t                   *part;
    ngx_uint_t                         i, found, known;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_cache_purge_module);

    for (status = cmcf->statuses; status != NULL; status = status->next) {
        found = 0;
        for (part = &cf->cycle->shared_memory.part;
             part != NULL && !found; part = part->next)
        {
            zones = part->elts;
            for (i = 0; i < part->nelts; i++) {
                known = 0;
# if (NGX_HTTP_PROXY)
                known |= (zones[i].tag == &ngx_http_proxy_module);
# endif
# if (NGX_HTTP_FASTCGI)
                known |= (zones[i].tag == &ngx_http_fastcgi_module);
# endif
# if (NGX_HTTP_SCGI)
                known |= (zones[i].tag == &ngx_http_scgi_module);
# endif
# if (NGX_HTTP_UWSGI)
                known |= (zones[i].tag == &ngx_http_uwsgi_module);
# endif
                if (!known || zones[i].data == NULL) {
                    continue;
                }
                cache = zones[i].data;
                if (cache->path->name.len == status->name.len
                    && ngx_filename_cmp(cache->path->name.data,
                                        status->name.data, status->name.len)
                       == 0)
                {
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                              "cache_purge_status: unknown cache path "
                              "\"%V\"", &status->name);
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


/* -- response type directive -------------------------------------------- */

char *
ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_loc_conf_t *cplcf = conf;
    ngx_str_t                       *value;

    (void) cmd;

    if (cplcf->response_type != NGX_CONF_UNSET_UINT) {
        return "is duplicate";
    }

    if (cf->args->nelts != 2) {
        return "requires exactly one argument: html|json|xml|text";
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
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid parameter \"%V\", expected html|json|xml|text",
            &value[1]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/* -- access control ----------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_access_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_loc_conf_t *cplcf;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /*
     * Belt-and-suspenders: the merge logic only installs this handler when
     * conf->conf is set, so this should never be NULL in production.  Guard
     * anyway to eliminate the crash class entirely.
     */
    if (cplcf->conf == NULL) {
        return NGX_HTTP_NOT_FOUND;
    }

    if (r->method_name.len != cplcf->conf->method.len
        || ngx_strncmp(r->method_name.data, cplcf->conf->method.data,
                       r->method_name.len) != 0)
    {
        /*
         * Not a purge request.  Forward to the original content handler
         * if one exists (e.g. proxy_pass), otherwise return 404.
         * original_handler is NULL when proxy_cache is used without
         * proxy_pass (cache-only / purge-only location).
         */
        if (cplcf->original_handler != NULL) {
            return cplcf->original_handler(r);
        }
        return NGX_HTTP_NOT_FOUND;
    }

    if ((cplcf->conf->access || cplcf->conf->access6)
        && ngx_http_cache_purge_access(cplcf->conf->access,
                                       cplcf->conf->access6,
                                       r->connection->sockaddr) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_cache_purge: access denied for %V",
                      &r->connection->addr_text);
        return NGX_HTTP_FORBIDDEN;
    }

    if (cplcf->handler == NULL) {
        return NGX_HTTP_NOT_FOUND;
    }

    return cplcf->handler(r);
}

ngx_int_t
ngx_http_cache_purge_access(ngx_array_t *access, ngx_array_t *access6,
    struct sockaddr *s)
{
    in_addr_t        inaddr;
    ngx_in_cidr_t   *a;
    ngx_uint_t       i;
# if (NGX_HAVE_INET6)
    struct in6_addr *inaddr6;
    ngx_in6_cidr_t  *a6;
    u_char          *p;
    ngx_uint_t       n;
# endif

    (void) access6;

    switch (s->sa_family) {
    case AF_INET:
        if (access == NULL) {
            return NGX_DECLINED;
        }

        inaddr = ((struct sockaddr_in *) s)->sin_addr.s_addr;

# if (NGX_HAVE_INET6)
ipv4:
# endif
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
        p       = inaddr6->s6_addr;

        if (access && IN6_IS_ADDR_V4MAPPED(inaddr6)) {
            inaddr  = p[12] << 24;
            inaddr += p[13] << 16;
            inaddr += p[14] << 8;
            inaddr += p[15];
            inaddr  = htonl(inaddr);
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
# endif
    }

    return NGX_DECLINED;
}


/* -- response builder --------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_send_response(ngx_http_request_t *r, ngx_str_t *status)
{
    ngx_http_cache_purge_loc_conf_t *cplcf;
    ngx_chain_t                      out;
    ngx_buf_t                       *b;
    ngx_str_t                       *key;
    ngx_int_t                        rc;
    size_t                           body_len;
    u_char                          *buf, *buf_keydata;
    const char                      *resp_ct,   *resp_body;
    size_t                           resp_ct_size, resp_body_size;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    key   = r->cache->keys.elts;

    buf_keydata = ngx_pcalloc(r->pool, key[0].len + 1);
    if (buf_keydata == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_memcpy(buf_keydata, key[0].data, key[0].len);
    /* buf_keydata[key[0].len] is already '\0' from ngx_pcalloc */

    switch (cplcf->response_type) {
    case NGX_CACHE_PURGE_RESPONSE_TYPE_JSON:
        resp_ct        = ngx_http_cache_purge_content_type_json;
        resp_ct_size   = ngx_http_cache_purge_content_type_json_size;
        resp_body      = ngx_http_cache_purge_body_templ_json;
        resp_body_size = ngx_http_cache_purge_body_templ_json_size;
        break;
    case NGX_CACHE_PURGE_RESPONSE_TYPE_XML:
        resp_ct        = ngx_http_cache_purge_content_type_xml;
        resp_ct_size   = ngx_http_cache_purge_content_type_xml_size;
        resp_body      = ngx_http_cache_purge_body_templ_xml;
        resp_body_size = ngx_http_cache_purge_body_templ_xml_size;
        break;
    case NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT:
        resp_ct        = ngx_http_cache_purge_content_type_text;
        resp_ct_size   = ngx_http_cache_purge_content_type_text_size;
        resp_body      = ngx_http_cache_purge_body_templ_text;
        resp_body_size = ngx_http_cache_purge_body_templ_text_size;
        break;
    default:
    case NGX_CACHE_PURGE_RESPONSE_TYPE_HTML:
        resp_ct        = ngx_http_cache_purge_content_type_html;
        resp_ct_size   = ngx_http_cache_purge_content_type_html_size;
        resp_body      = ngx_http_cache_purge_body_templ_html;
        resp_body_size = ngx_http_cache_purge_body_templ_html_size;
        break;
    }

    /*
     * Compute the rendered output length.
     *
     * resp_body_size = sizeof(template_string) which includes the NUL
     * terminator appended by the compiler to every string literal.  Each
     * body template contains exactly two "%s" format specifiers (2 bytes
     * each) that ngx_snprintf replaces with the cache key and the status
     * word respectively.  The rendered output length is therefore:
     *
     *   body_len = sizeof(template)
     *              - 1           (NUL terminator is not sent on the wire)
     *              - (2 * 2)     (two "%s" markers consumed, not emitted)
     *              + key[0].len  (first  %s expansion)
     *              + status->len (second %s expansion)
     *
     * Simplified: (resp_body_size - 5) + key.len + status.len
     *
     * ngx_snprintf writes exactly body_len bytes without a NUL terminator
     * (it stops at buf + max, exclusive).  buf is ngx_pcalloc'd to
     * body_len + 1 so the trailing zero from calloc is there for any code
     * that treats buf as a C string, but it is never sent over the wire.
     */
    body_len = (resp_body_size - 1 - 4) + key[0].len + status->len;

    r->headers_out.content_type.len  = resp_ct_size - 1;
    r->headers_out.content_type.data = (u_char *) resp_ct;

    buf = ngx_pcalloc(r->pool, body_len + 1);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* ngx_snprintf never returns NULL */
    ngx_snprintf(buf, body_len, resp_body, buf_keydata, status->data);

    r->headers_out.status           = (r->headers_out.status == NGX_HTTP_ACCEPTED)
                                      ? NGX_HTTP_ACCEPTED : NGX_HTTP_OK;
    r->headers_out.content_length_n = (off_t) body_len;

    b = ngx_create_temp_buf(r->pool, body_len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf  = b;
    out.next = NULL;

    b->last     = ngx_cpymem(b->last, buf, body_len);
    b->last_buf = 1;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


/* -- cache get helper (nginx >= 1.7.9) --------------------------------- */

# if (nginx_version >= 1007009)
ngx_int_t
ngx_http_cache_purge_cache_get(ngx_http_request_t *r, ngx_http_upstream_t *u,
    ngx_http_file_cache_t **cache)
{
    ngx_str_t              *name;
    ngx_str_t               val;
    ngx_uint_t              i;
    ngx_http_file_cache_t **caches;

    if (u->conf->cache_zone) {
        *cache = u->conf->cache_zone->data;
        return NGX_OK;
    }

    if (u->conf->cache_value == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_cache_purge: no cache configured for this location");
        return NGX_HTTP_NOT_FOUND;
    }

    if (ngx_http_complex_value(r, u->conf->cache_value, &val) != NGX_OK) {
        return NGX_ERROR;
    }

    if (val.len == 0
        || (val.len == 3 && ngx_strncmp(val.data, "off", 3) == 0))
    {
        return NGX_DECLINED;
    }

    caches = u->caches->elts;

    for (i = 0; i < u->caches->nelts; i++) {
        name = &caches[i]->shm_zone->shm.name;
        if (name->len == val.len
            && ngx_strncmp(name->data, val.data, val.len) == 0)
        {
            *cache = caches[i];
            return NGX_OK;
        }
    }

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "ngx_cache_purge: cache zone \"%V\" not found", &val);

    return NGX_ERROR;
}
# endif


/* -- request init ------------------------------------------------------- */

ngx_int_t
ngx_http_cache_purge_init(ngx_http_request_t *r, ngx_http_file_cache_t *cache,
    ngx_http_complex_value_t *cache_key)
{
    ngx_http_cache_t *c;
    ngx_str_t        *key;
    ngx_int_t         rc;

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

    r->cache      = c;
    c->body_start = ngx_pagesize;
    c->file_cache = cache;
    c->file.log   = r->connection->log;

    ngx_http_file_cache_create_key(r);

    return NGX_OK;
}


/* -- purge dispatch ----------------------------------------------------- */

void
ngx_http_cache_purge_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_main_conf_t *cmcf;
    ngx_str_t                         status;
    ngx_int_t                         rc;
    ngx_int_t                         not_found_code;

# if (NGX_HAVE_FILE_AIO)
    if (r->aio) {
        return;
    }
# endif

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);

    not_found_code = (cmcf != NULL && cmcf->legacy_status_codes)
                     ? NGX_HTTP_PRECONDITION_FAILED
                     : NGX_HTTP_NOT_FOUND;

    rc = ngx_http_file_cache_purge(r);

    switch (rc) {
    case NGX_OK:
        ngx_str_set(&status, "purged");
        r->write_event_handler = ngx_http_request_empty_handler;

        /*
         * Vary-aware cleanup: after deleting the primary file, walk the
         * cache directory to remove any remaining variant files (e.g. those
         * created by gzip_vary / Vary: Accept-Encoding).  All variant files
         * share the same KEY: string, which is matched exactly by the walk.
         * r->cache->file_cache is set by ngx_http_cache_purge_init().
         */
        if (cmcf != NULL && cmcf->vary_aware) {
            ngx_http_cache_purge_delete_variants(r, r->cache->file_cache);
        }

        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return;

    case NGX_DECLINED:
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: key \"%V\" not found in cache",
                       (ngx_str_t *) r->cache->keys.elts);
        ngx_http_finalize_request(r, not_found_code);
        return;

# if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        r->write_event_handler = ngx_http_cache_purge_handler;
        return;
# endif

    default:
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
    }
}


/* -- file cache purge --------------------------------------------------- */

ngx_int_t
ngx_http_file_cache_purge(ngx_http_request_t *r)
{
    ngx_http_file_cache_t *cache;
    ngx_http_cache_t      *c;

    switch (ngx_http_file_cache_open(r)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
# if (nginx_version >= 8001) \
     || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
# endif
        break;

    case NGX_DECLINED:
        return NGX_DECLINED;

# if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
        return NGX_AGAIN;
# endif

    default:
        return NGX_ERROR;
    }

    c     = r->cache;
    cache = c->file_cache;

    ngx_shmtx_lock(&cache->shpool->mutex);

    if (!c->node->exists) {
        ngx_shmtx_unlock(&cache->shpool->mutex);
        return NGX_DECLINED;
    }

# if (nginx_version >= 1000001)
    cache->sh->size -= c->node->fs_size;
    c->node->fs_size  = 0;
# else
    cache->sh->size -= (c->node->length + cache->bsize - 1) / cache->bsize;
    c->node->length   = 0;
# endif

    c->node->exists = 0;
# if (nginx_version >= 8001) \
     || ((nginx_version < 8000) && (nginx_version >= 7060))
    c->node->updating = 0;
# endif

    ngx_shmtx_unlock(&cache->shpool->mutex);

    if (ngx_delete_file(c->file.name.data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                      "ngx_cache_purge: could not delete \"%V\"", &c->file.name);
    } else {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: deleted \"%V\"", &c->file.name);
    }

    return NGX_OK;
}


/* -- bulk walk helpers -------------------------------------------------- */

void
ngx_http_cache_purge_all(ngx_http_request_t *r, ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_walk_ctx_t  ctx;
    ngx_tree_ctx_t                   tree;

    ngx_memzero(&ctx,  sizeof(ngx_http_cache_purge_walk_ctx_t));
    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));

    ctx.root_len = cache->path->name.len;

    tree.file_handler      = ngx_http_purge_file_cache_delete_file;
    tree.pre_tree_handler  = ngx_http_cache_purge_pre_tree;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler      = ngx_http_purge_file_cache_noop;
    tree.data              = &ctx;
    tree.log               = ngx_cycle->log;

    ngx_walk_tree(&tree, &cache->path->name);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ngx_cache_purge: purge_all deleted %ui file(s) "
                   "from zone \"%V\"", ctx.files_deleted, &cache->path->name);
}

ngx_uint_t
ngx_http_cache_purge_partial(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_walk_ctx_t  ctx;
    ngx_tree_ctx_t                   tree;
    ngx_str_t                       *key;
    ngx_uint_t                       len;

    key = r->cache->keys.elts;
    len = key[0].len;

    if (len > 0 && key[0].data[len - 1] == '*') {
        len--;
    }

    ngx_memzero(&ctx,  sizeof(ngx_http_cache_purge_walk_ctx_t));
    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));

    ctx.key_partial = key[0].data;
    ctx.key_len     = len;
    ctx.root_len    = cache->path->name.len;

    tree.file_handler      = ngx_http_purge_file_cache_delete_partial_file;
    tree.pre_tree_handler  = ngx_http_cache_purge_pre_tree;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler      = ngx_http_purge_file_cache_noop;
    tree.data              = &ctx;
    tree.log               = ngx_cycle->log;

    ngx_walk_tree(&tree, &cache->path->name);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ngx_cache_purge: partial walk deleted %ui file(s) "
                   "for key prefix \"%V\"", ctx.files_deleted, &key[0]);

    return ctx.files_deleted;
}

ngx_int_t
ngx_http_cache_purge_is_partial(ngx_http_request_t *r)
{
    ngx_http_cache_t *c   = r->cache;
    ngx_str_t        *key = c->keys.elts;

    return c->keys.nelts > 0
        && key[0].len > 0
        && key[0].data[key[0].len - 1] == '*';
}


/* -- configuration parser ----------------------------------------------- */

char *
ngx_http_cache_purge_conf(ngx_conf_t *cf, ngx_http_cache_purge_conf_t *cpcf)
{
    ngx_cidr_t      cidr;
    ngx_in_cidr_t  *access;
# if (NGX_HAVE_INET6)
    ngx_in6_cidr_t *access6;
# endif
    ngx_str_t      *value;
    ngx_int_t       rc;
    ngx_uint_t      i, from_position;

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
            "invalid parameter \"%V\", expected \"from\" keyword",
            &value[from_position]);
        return NGX_CONF_ERROR;
    }

    if (ngx_strcmp(value[from_position + 1].data, "all") == 0) {
        cpcf->enable = 1;
        return NGX_CONF_OK;
    }

    for (i = from_position + 1; i < cf->args->nelts; i++) {
        rc = ngx_ptocidr(&value[i], &cidr);

        if (rc == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "invalid parameter \"%V\"", &value[i]);
            return NGX_CONF_ERROR;
        }

        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                "low address bits of %V are meaningless", &value[i]);
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

void *
ngx_http_cache_purge_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_cache_purge_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_cache_purge_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

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
    ngx_http_cache_purge_loc_conf_t *prev = parent;
    ngx_http_cache_purge_loc_conf_t *conf = child;
    ngx_http_core_loc_conf_t        *clcf;
    /*
     * C89: all variables at top of function.
     *
     * was_set_* captures whether each protocol's purge directive was
     * explicitly present in THIS location block BEFORE merging from the
     * parent.  Together with clcf->noname it distinguishes three cases:
     *
     *   Case A -- explicit (enable == 1 before merge):
     *     proxy_cache_purge is in this location.  clcf->handler is the
     *     real upstream handler (e.g. ngx_http_proxy_handler set by
     *     proxy_pass).  Save it as original_handler and install ours.
     *
     *   Case B -- inherited into a named location (enable ==
     *     NGX_CONF_UNSET, clcf->noname == 0):
     *     proxy_cache_purge is set at the server level or in an enclosing
     *     location.  clcf->handler is this location's own handler, as in
     *     case A.
     *
     *   Case C -- inherited into an anonymous location (enable ==
     *     NGX_CONF_UNSET, clcf->noname == 1):
     *     This is an anonymous if-child location synthesised by nginx when
     *     it encounters an "if" block.  The if-block has no handler
     *     directive, so clcf->handler is NULL.  Saving NULL as
     *     original_handler causes every non-PURGE request that enters the
     *     if-branch to return 404 instead of reaching the upstream.
     *     Inherit original_handler from prev (which holds the real handler
     *     saved during the parent location's merge) instead.
     *
     * In both cases clcf->handler must be set to access_handler so that
     * PURGE requests are intercepted regardless of whether the if condition
     * fires.
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

    if (clcf->noname && conf->status == NULL) {
        conf->status = prev->status;
    }
    if (conf->status != NULL) {
        if (clcf->handler != NULL
            && clcf->handler != ngx_http_cache_purge_status_handler)
        {
            return "cache_purge_status conflicts with the content handler";
        }
        clcf->handler = ngx_http_cache_purge_status_handler;
        return NGX_CONF_OK;
    }

    ngx_conf_merge_uint_value(conf->response_type, prev->response_type,
                              NGX_CACHE_PURGE_RESPONSE_TYPE_HTML);

# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_merge_conf(&conf->fastcgi, &prev->fastcgi);

    if (conf->fastcgi.enable) {
        conf->conf             = &conf->fastcgi;
        conf->handler          = ngx_http_fastcgi_cache_purge_handler;
        conf->original_handler = (was_set_fastcgi || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

# if (NGX_HTTP_PROXY)
    if (conf->proxy.enable == NGX_CONF_UNSET
        && conf->proxy_separate_zone == NULL
        && conf->proxy_separate_value == NULL)
    {
        conf->proxy_separate_zone  = prev->proxy_separate_zone;
        conf->proxy_separate_value = prev->proxy_separate_value;
        conf->proxy_separate_key   = prev->proxy_separate_key;
    }

    ngx_http_cache_purge_merge_conf(&conf->proxy, &prev->proxy);

    if (conf->proxy.enable) {
        conf->conf             = &conf->proxy;
        conf->handler          = ngx_http_proxy_cache_purge_handler;
        conf->original_handler = (was_set_proxy || !clcf->noname)
                                 ? clcf->handler
                                 : prev->original_handler;
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
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
        return NGX_CONF_OK;
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
        return NGX_CONF_OK;
    }
# endif

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
