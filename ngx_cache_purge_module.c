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


#ifndef nginx_version
# error This module cannot be built against an unknown nginx version.
#endif

#define NGX_REPONSE_TYPE_HTML  1
#define NGX_REPONSE_TYPE_XML   2
#define NGX_REPONSE_TYPE_JSON  3
#define NGX_REPONSE_TYPE_TEXT  4

#define NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT   1024
#define NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT   10
#define NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT  10
#define NGX_CACHE_PURGE_KEY_MAX_LEN          512
#define NGX_CACHE_PURGE_QUEUE_TIMEOUT        60000   /* ms */

/*
 * Minimum shared-memory size for the background queue, expressed in pages.
 * The slab allocator consumes an amount of metadata (pool header, slot
 * descriptors, stat entries, page descriptors, and an alignment gap) that
 * varies with nginx version, build flags, and architecture.  Rather than
 * attempting to compute that overhead from internal slab structs — which
 * differ between nginx 1.8/1.9+, are affected by NGX_HAVE_POSIX_SEM /
 * --with-threads, and scale with the OS page size (4 KB on x86 Linux,
 * 8–64 KB on some *BSD / ARM / POWER platforms) — we simply enforce a
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

static size_t ngx_http_cache_purge_content_type_json_size =
    sizeof(ngx_http_cache_purge_content_type_json);
static size_t ngx_http_cache_purge_content_type_html_size =
    sizeof(ngx_http_cache_purge_content_type_html);
static size_t ngx_http_cache_purge_content_type_xml_size =
    sizeof(ngx_http_cache_purge_content_type_xml);
static size_t ngx_http_cache_purge_content_type_text_size =
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

static size_t ngx_http_cache_purge_body_templ_json_size =
    sizeof(ngx_http_cache_purge_body_templ_json);
static size_t ngx_http_cache_purge_body_templ_html_size =
    sizeof(ngx_http_cache_purge_body_templ_html);
static size_t ngx_http_cache_purge_body_templ_xml_size =
    sizeof(ngx_http_cache_purge_body_templ_xml);
static size_t ngx_http_cache_purge_body_templ_text_size =
    sizeof(ngx_http_cache_purge_body_templ_text);


#if (NGX_HTTP_CACHE)

/* -- forward declarations ----------------------------------------------- */

typedef struct ngx_http_cache_purge_queue_item_s ngx_http_cache_purge_queue_item_t;
typedef struct ngx_http_cache_purge_queue_s      ngx_http_cache_purge_queue_t;
typedef struct ngx_http_cache_purge_main_conf_s  ngx_http_cache_purge_main_conf_t;


/* -- data structures ---------------------------------------------------- */

struct ngx_http_cache_purge_queue_item_s {
    ngx_str_t                          cache_path;
    ngx_str_t                          key_partial;
    ngx_uint_t                         hash;
    ngx_flag_t                         purge_all;
    ngx_uint_t                         in_progress; /* reserved for ABI stability */
    ngx_msec_t                         enqueued_at;
    ngx_http_cache_purge_queue_item_t *next;
};

struct ngx_http_cache_purge_queue_s {
    ngx_http_cache_purge_queue_item_t *head;
    ngx_http_cache_purge_queue_item_t *tail;
    ngx_atomic_t                       size;
    ngx_shmtx_sh_t                     sh;
    ngx_shmtx_t                        mutex;
    ngx_slab_pool_t                   *shpool;
    ngx_uint_t                         max_size;
    ngx_uint_t                         batch_size;
    ngx_msec_t                         throttle_ms;
};

struct ngx_http_cache_purge_main_conf_s {
    ngx_http_cache_purge_queue_t      *queue;
    ngx_shm_zone_t                    *shm_zone;
    ngx_uint_t                         queue_size;
    ngx_uint_t                         batch_size;
    ngx_msec_t                         throttle_ms;
    ngx_flag_t                         background_purge;
    ngx_flag_t                         legacy_status_codes;
    /*
     * vary_aware: when on, an exact-key purge walks the cache directory after
     * deleting the primary file and removes any remaining files that carry the
     * same KEY: string (i.e. Vary / gzip_vary variants at different paths).
     * Disabled by default because it adds a full cache walk per purge request.
     */
    ngx_flag_t                         vary_aware;
};

typedef struct {
    ngx_flag_t                    enable;
    ngx_str_t                     method;
    ngx_flag_t                    purge_all;
    ngx_flag_t                    refresh;
    ngx_array_t                  *access;   /* array of ngx_in_cidr_t */
    ngx_array_t                  *access6;  /* array of ngx_in6_cidr_t */
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

    ngx_http_cache_purge_conf_t *conf;
    ngx_http_handler_pt          handler;
    ngx_http_handler_pt          original_handler;
    ngx_uint_t                   resptype;

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
    ngx_uint_t                    refresh_concurrency;
    ngx_msec_t                    refresh_timeout;
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    u_char     *key_partial;
    ngx_uint_t  key_len;
    u_char      key_buffer[NGX_CACHE_PURGE_KEY_MAX_LEN];
    ngx_uint_t  files_deleted;
    ngx_uint_t  files_checked;
} ngx_http_cache_purge_walk_ctx_t;


/* -- function prototypes ------------------------------------------------ */

static void *ngx_http_cache_purge_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_cache_purge_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone,
    void *data);
static ngx_int_t ngx_http_cache_purge_init_worker(ngx_cycle_t *cycle);
static void ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle);
static void ngx_http_cache_purge_background_handler(ngx_event_t *ev);
static ngx_int_t ngx_http_cache_purge_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all);
static ngx_int_t ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle);
static ngx_uint_t ngx_http_cache_purge_hash_key(ngx_str_t *cache_path,
    ngx_str_t *key);
static ngx_http_cache_purge_queue_item_t *ngx_http_cache_purge_find_duplicate(
    ngx_http_cache_purge_queue_t *queue, ngx_uint_t hash,
    ngx_str_t *cache_path, ngx_str_t *key);

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
static ngx_int_t ngx_http_cache_purge_invalidate_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_cache_purge_invalidate_partial_file(
    ngx_tree_ctx_t *ctx, ngx_str_t *path);

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

typedef enum {
    NGX_HTTP_CACHE_PURGE_INVALIDATE_PURGED = 0,
    NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_MISSING,
    NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_REPLACED,
    NGX_HTTP_CACHE_PURGE_INVALIDATE_ERROR
} ngx_http_cache_purge_invalidate_result_e;

typedef struct {
    ngx_str_t    cache_key;
    ngx_str_t    cache_path;
    ngx_file_uniq_t uniq;
    time_t       last_modified;
    u_short      body_start;
    u_char       etag_len;
    u_char       etag[NGX_HTTP_CACHE_ETAG_LEN];
    off_t        fs_size;
} ngx_http_cache_purge_invalidate_item_t;

typedef struct {
    ngx_http_request_t       *request;
    ngx_http_file_cache_t    *cache;
    ngx_str_t                 partial_prefix;
    ngx_flag_t                match_all;
    ngx_uint_t                files_deleted;
    ngx_queue_t               temp_pools;
} ngx_http_cache_purge_batch_ctx_t;

static ngx_int_t ngx_http_cache_purge_read_item(ngx_pool_t *pool,
    ngx_log_t *log, ngx_str_t *path,
    ngx_http_cache_purge_invalidate_item_t *item);
static ngx_int_t ngx_http_cache_purge_enqueue_temp_pool(ngx_queue_t *queue,
    ngx_pool_t *owner_pool, ngx_pool_t *pool);
static void ngx_http_cache_purge_drain_temp_pools(ngx_queue_t *queue);
static ngx_int_t ngx_http_cache_purge_invalidate_opened_cache(ngx_log_t *log,
    ngx_http_file_cache_t *cache, ngx_http_cache_t *c,
    ngx_pool_t *pool, ngx_http_cache_purge_invalidate_item_t *item,
    ngx_http_cache_purge_invalidate_result_e *result);
static ngx_int_t ngx_http_cache_purge_open_temp_cache(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_pool_t *pool, ngx_str_t *cache_key,
    ngx_http_cache_t *c);
static ngx_int_t ngx_http_cache_purge_item_matches_cache(
    ngx_http_cache_purge_invalidate_item_t *expected,
    ngx_http_cache_t *c);
static ngx_int_t ngx_http_cache_purge_cache_matches_node(
    ngx_http_cache_t *c);
static ngx_int_t ngx_http_cache_purge_invalidate_item(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_pool_t *pool,
    ngx_http_cache_purge_invalidate_item_t *item,
    ngx_http_cache_purge_invalidate_result_e *result);

typedef struct {
    ngx_str_t    uri;
    ngx_str_t    args;
    ngx_str_t    etag;
    time_t       last_modified;
    ngx_str_t    path;
    ngx_http_cache_purge_invalidate_item_t item;
} ngx_http_cache_purge_refresh_file_t;

typedef struct {
    ngx_str_t     path;
    ngx_flag_t    is_dir;
} ngx_http_cache_purge_refresh_scan_entry_t;

typedef struct {
    ngx_queue_t    queue;
    ngx_pool_t    *pool;
} ngx_http_cache_purge_refresh_temp_pool_t;

typedef struct {
    ngx_queue_t    queue;
    ngx_str_t      path;
} ngx_http_cache_purge_refresh_dir_t;

typedef struct {
    ngx_http_request_t              *request;       /* parent request */
    ngx_http_file_cache_t           *cache;         /* cache instance */
    ngx_str_t                        key_partial;   /* key prefix (without *) */
    ngx_uint_t                       key_prefix_len;/* non-URI prefix length in key */
    ngx_flag_t                       purge_all;
    ngx_flag_t                       exact;
    ngx_flag_t                       timed_out;
    ngx_flag_t                       timeout_enabled;
    ngx_flag_t                       finalized;
    ngx_msec_t                       deadline;
    ngx_event_t                      timeout_ev;

    /* collected refresh candidates */
    ngx_pool_t                      *chunk_pool;
    ngx_pool_t                      *retired_chunk_pool;
    ngx_queue_t                      retired_chunk_pools;
    ngx_pool_t                      *scan_pool;
    ngx_array_t                     *files;         /* collected files: ngx_http_cache_purge_refresh_file_t[] */
    ngx_array_t                     *scan_entries;  /* current directory entries */
    ngx_uint_t                       current;       /* next file index to dispatch */
    ngx_uint_t                       queued;        /* total collected file count */
    ngx_uint_t                       active;        /* active subrequest count */
    ngx_uint_t                       dispatched;    /* total dispatched subrequests */
    ngx_uint_t                       chunk_limit;
    ngx_uint_t                       scan_index;
    ngx_flag_t                       scan_done;     /* collection complete */
    ngx_flag_t                       scan_initialized;
    ngx_queue_t                      temp_pools;
    ngx_queue_t                      pending_dirs;
    /* stats */
    ngx_uint_t                       total;
    ngx_uint_t                       refreshed;
    ngx_uint_t                       purged;
    ngx_uint_t                       errors;
} ngx_http_cache_purge_refresh_ctx_t;

typedef struct {
    ngx_http_cache_purge_refresh_ctx_t  *ctx;
    ngx_http_cache_purge_refresh_file_t *file;
    ngx_flag_t                           validation_ready;
    ngx_flag_t                           handled;
} ngx_http_cache_purge_refresh_post_data_t;

static ngx_int_t ngx_http_cache_purge_refresh(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache);
static ngx_int_t ngx_http_cache_purge_refresh_collect_open_file(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_collect_path(
    ngx_http_cache_purge_refresh_ctx_t *rctx, ngx_str_t *path,
    ngx_uint_t exact_match);
static void ngx_http_cache_purge_refresh_start(ngx_http_request_t *r);
static ngx_int_t ngx_http_cache_purge_refresh_fire_subrequest(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_done(
    ngx_http_request_t *r, void *data, ngx_int_t rc);
static ngx_int_t ngx_http_cache_purge_refresh_enqueue_retired_chunk_pool(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_pool_t *pool);
static void ngx_http_cache_purge_refresh_drain_retired_chunk_pools(
    ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_send_response(
    ngx_http_request_t *r);
static void ngx_http_cache_purge_refresh_timeout_handler(ngx_event_t *ev);
static void ngx_http_cache_purge_refresh_mark_timeout(
    ngx_http_cache_purge_refresh_ctx_t *ctx);
static void ngx_http_cache_purge_refresh_pool_cleanup(void *data);
static void ngx_http_cache_purge_refresh_finalize(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_scan_next_chunk(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx);
static ngx_int_t ngx_http_cache_purge_refresh_enqueue_dir(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_str_t *path);
static ngx_int_t ngx_http_cache_purge_refresh_load_dir(
    ngx_http_request_t *r, ngx_http_cache_purge_refresh_ctx_t *ctx,
    ngx_str_t *path);
static ngx_int_t ngx_http_cache_purge_add_variable(ngx_conf_t *cf);
static ngx_int_t ngx_http_cache_purge_refresh_bypass_variable(
    ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);


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

    { ngx_string("cache_purge_refresh_concurrency"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_loc_conf_t, refresh_concurrency),
      NULL },

    { ngx_string("cache_purge_refresh_timeout"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_cache_purge_loc_conf_t, refresh_timeout),
      NULL },

    ngx_null_command
};


/* -- module context & descriptor ---------------------------------------- */

static ngx_http_module_t  ngx_http_cache_purge_module_ctx = {
    ngx_http_cache_purge_add_variable,      /* preconfiguration  */
    NULL,                                   /* postconfiguration */
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

/* Per-worker globals — safe because nginx forks one process per worker */
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

    ngx_conf_init_value(cmcf->background_purge,    0);
    ngx_conf_init_uint_value(cmcf->queue_size,     NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT);
    ngx_conf_init_uint_value(cmcf->batch_size,     NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT);
    ngx_conf_init_msec_value(cmcf->throttle_ms,    NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT);
    /* Default on: return 412 for missing entries (backwards compatibility) */
    ngx_conf_init_value(cmcf->legacy_status_codes, 1);
    /* Default off: vary-aware walk adds cost; opt in explicitly */
    ngx_conf_init_value(cmcf->vary_aware,          0);

    if (!cmcf->background_purge) {
        return NGX_CONF_OK;
    }

    shm_size = sizeof(ngx_http_cache_purge_queue_t)
             + cmcf->queue_size * sizeof(ngx_http_cache_purge_queue_item_t)
             + cmcf->queue_size * 2 * NGX_CACHE_PURGE_KEY_MAX_LEN;

    /*
     * The slab allocator imposes metadata overhead that is NOT reflected in
     * the payload calculation above:
     *
     *   ngx_slab_pool_t header  — size varies by nginx version, build flags
     *                             (e.g. NGX_HAVE_POSIX_SEM, --with-threads),
     *                             and platform ABI.
     *   Slot descriptors        — (pagesize_shift - min_shift) page structs.
     *   Stat entries            — same count, added in nginx ~1.9.x.
     *   Page descriptors        — one per allocatable page.
     *   Alignment gap           — up to one full page between descriptors
     *                             and the first usable byte (pool->start is
     *                             rounded up to the next page boundary).
     *
     * Attempting to compute this precisely requires knowledge of internal
     * nginx structs that differ across versions (1.8 vs 1.9+), build
     * configurations, and architectures (Linux, *BSD, Solaris, macOS —
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

    cmcf->shm_zone = ngx_shared_memory_add(cf, &name, shm_size,
                                           &ngx_http_cache_purge_module);
    if (cmcf->shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    cmcf->shm_zone->init = ngx_http_cache_purge_init_shm_zone;
    cmcf->shm_zone->data = cmcf;

    return NGX_CONF_OK;
}

/*
 * FIX: on a live reload `data` is the previous cycle's main_conf.
 * Propagate the existing queue pointer so new workers don't start with
 * queue==NULL.
 */
static ngx_int_t
ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_cache_purge_main_conf_t *cmcf   = shm_zone->data;
    ngx_http_cache_purge_main_conf_t *old    = data;
    ngx_http_cache_purge_queue_t     *queue;
    ngx_slab_pool_t                  *shpool;

    if (old != NULL) {
        cmcf->queue = old->queue;
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    queue = ngx_slab_calloc(shpool, sizeof(ngx_http_cache_purge_queue_t));
    if (queue == NULL) {
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                      "ngx_cache_purge: could not allocate queue "
                      "in shared memory zone \"%V\"", &shm_zone->shm.name);
        return NGX_ERROR;
    }

    queue->shpool      = shpool;
    queue->max_size    = cmcf->queue_size;
    queue->batch_size  = cmcf->batch_size;
    queue->throttle_ms = cmcf->throttle_ms;

    if (ngx_shmtx_create(&queue->mutex, &queue->sh, NULL) != NGX_OK) {
        return NGX_ERROR;
    }

    cmcf->queue = queue;

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
    if (cmcf == NULL || !cmcf->background_purge) {
        return NGX_OK;
    }

    ngx_cache_purge_main_conf = cmcf;

    ngx_memzero(&ngx_cache_purge_event, sizeof(ngx_event_t));
    ngx_cache_purge_event.handler     = ngx_http_cache_purge_background_handler;
    ngx_cache_purge_event.log         = cycle->log;
    ngx_cache_purge_event.data        = cycle;
    /*
     * Mark the timer as cancelable (nginx >= 1.11.11, June 2017).
     * Without this flag nginx's graceful-shutdown path waits for every
     * pending timer to fire before allowing the worker to exit.  Because
     * this handler re-arms itself on every invocation the worker would
     * never exit cleanly, causing Test::Nginx (and real deployments) to
     * time out and fall back to SIGKILL.  "cancelable" tells the event
     * loop: "discard this timer when the worker is exiting — do not wait
     * for it."  All nginx versions we support (≥ 1.20) have this field.
     */
    ngx_cache_purge_event.cancelable  = 1;

    ngx_add_timer(&ngx_cache_purge_event, cmcf->throttle_ms);

    return NGX_OK;
}

static void
ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle)
{
    if (ngx_cache_purge_event.timer_set) {
        ngx_del_timer(&ngx_cache_purge_event);
    }
}

/*
 * Background timer callback — fires every throttle_ms milliseconds.
 *
 * Processes ONE queue item per invocation then yields back to the nginx
 * event loop.  The timer re-arms with throttle_ms after each item walk,
 * and with throttle_ms * 10 when the queue is empty (backoff).
 *
 * This replaces the previous pattern where a batch of items were all
 * walked inside a single callback, with ngx_msleep() called every 100
 * files to throttle I/O.  ngx_msleep() is a literal usleep() call: it
 * blocks the OS thread, stalling every connection that worker is handling
 * for the duration of the sleep.  Yielding via the event loop timer is
 * the correct nginx idiom.
 */
static void
ngx_http_cache_purge_background_handler(ngx_event_t *ev)
{
    ngx_cycle_t                      *cycle = ev->data;
    ngx_http_cache_purge_main_conf_t *cmcf  = ngx_cache_purge_main_conf;
    ngx_int_t                         rc;
    ngx_msec_t                        next_delay;

    if (cmcf == NULL || cmcf->queue == NULL) {
        ngx_add_timer(ev, NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT);
        return;
    }

    rc = ngx_http_cache_purge_process_queue(cycle);

    /*
     * NGX_AGAIN  → an item was processed and more remain; come back soon.
     * NGX_OK     → queue is now empty; back off to avoid busy-polling.
     * NGX_ERROR  → configuration problem; back off.
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
    ngx_uint_t                          hash;
    u_char                             *p;

    cmcf = ngx_http_get_module_main_conf(r, ngx_http_cache_purge_module);
    if (cmcf == NULL || cmcf->queue == NULL) {
        return NGX_ERROR;
    }

    queue = cmcf->queue;
    hash  = ngx_http_cache_purge_hash_key(&cache->path->name, key);

    ngx_shmtx_lock(&queue->mutex);

    if ((ngx_uint_t) queue->size >= queue->max_size) {
        ngx_shmtx_unlock(&queue->mutex);
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                      "ngx_cache_purge: queue full (%ui/%ui items), "
                      "falling back to synchronous purge",
                      queue->size, queue->max_size);
        return NGX_ERROR;
    }

    if (ngx_http_cache_purge_find_duplicate(queue, hash,
                                            &cache->path->name, key) != NULL)
    {
        ngx_shmtx_unlock(&queue->mutex);
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "ngx_cache_purge: duplicate enqueue for \"%V\" "
                       "key \"%V\", skipping", &cache->path->name, key);
        return NGX_OK;
    }

    item = ngx_slab_calloc(queue->shpool,
                           sizeof(ngx_http_cache_purge_queue_item_t));
    if (item == NULL) {
        ngx_shmtx_unlock(&queue->mutex);
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                      "ngx_cache_purge: shared memory exhausted, "
                      "could not allocate queue item");
        return NGX_ERROR;
    }

    /* +1: NUL terminator required by opendir / ngx_walk_tree */
    p = ngx_slab_alloc(queue->shpool, cache->path->name.len + 1);
    if (p == NULL) {
        ngx_slab_free(queue->shpool, item);
        ngx_shmtx_unlock(&queue->mutex);
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                      "ngx_cache_purge: shared memory exhausted, "
                      "could not allocate cache path buffer");
        return NGX_ERROR;
    }
    ngx_memcpy(p, cache->path->name.data, cache->path->name.len);
    p[cache->path->name.len] = '\0';
    item->cache_path.data = p;
    item->cache_path.len  = cache->path->name.len;

    if (key->len > 0) {
        /* +1: NUL terminator for string comparisons */
        p = ngx_slab_alloc(queue->shpool, key->len + 1);
        if (p == NULL) {
            ngx_slab_free(queue->shpool, item->cache_path.data);
            ngx_slab_free(queue->shpool, item);
            ngx_shmtx_unlock(&queue->mutex);
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                          "ngx_cache_purge: shared memory exhausted, "
                          "could not allocate key buffer");
            return NGX_ERROR;
        }
        ngx_memcpy(p, key->data, key->len);
        p[key->len] = '\0';
        item->key_partial.data = p;
        item->key_partial.len  = key->len;
    }

    item->hash        = hash;
    item->purge_all   = purge_all;
    item->in_progress = 0;
    item->enqueued_at = ngx_current_msec;

    if (queue->tail != NULL) {
        queue->tail->next = item;
    } else {
        queue->head = item;
    }
    queue->tail = item;
    queue->size++;

    ngx_shmtx_unlock(&queue->mutex);

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ngx_cache_purge: enqueued purge of \"%V\" key \"%V\" "
                   "(%ui item(s) in queue)",
                   &cache->path->name, key, queue->size);

    return NGX_OK;
}

/*
 * Batch-claim-then-process design:
 *
 * Each invocation dequeues and fully walks ONE item.  The background handler
 * re-arms the timer with throttle_ms after each call, giving the nginx event
 * loop a chance to handle connections between every walk.  This replaces the
 * previous ngx_msleep() anti-pattern, which blocked the OS thread (and all
 * connections on that worker) for throttle_ms × (files / 100) milliseconds
 * during every directory walk.
 *
 * Items are atomically removed from the queue head while the mutex is held,
 * then walked outside the lock.  This eliminates the stale-prev
 * use-after-free race that existed in the original unlock→walk→relock design.
 *
 * ngx_slab_free() acquires the slab pool's internal mutex, which is distinct
 * from the queue mutex.  Calling it outside the queue lock preserves the
 * consistent lock ordering: queue → slab.
 */
static ngx_int_t
ngx_http_cache_purge_process_queue(ngx_cycle_t *cycle)
{
    ngx_http_cache_purge_main_conf_t   *cmcf;
    ngx_http_cache_purge_queue_t       *queue;
    ngx_http_cache_purge_queue_item_t  *item;
    ngx_http_cache_purge_walk_ctx_t     ctx;
    ngx_tree_ctx_t                      tree;
    ngx_msec_t                          now;
    ngx_uint_t                          queue_remaining;

    cmcf = ngx_cache_purge_main_conf;
    if (cmcf == NULL || cmcf->queue == NULL) {
        return NGX_ERROR;
    }

    queue = cmcf->queue;
    now   = ngx_current_msec;
    item  = NULL;

    /* Dequeue exactly one item under the lock */
    ngx_shmtx_lock(&queue->mutex);

    while (queue->head != NULL) {
        item        = queue->head;
        queue->head = item->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
        queue->size--;
        item->next = NULL;

        if ((now - item->enqueued_at) > NGX_CACHE_PURGE_QUEUE_TIMEOUT) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                          "ngx_cache_purge: purge of \"%V\" key \"%V\" "
                          "timed out after %Mms, discarding",
                          &item->cache_path, &item->key_partial,
                          now - item->enqueued_at);
            ngx_slab_free(queue->shpool, item->cache_path.data);
            if (item->key_partial.data) {
                ngx_slab_free(queue->shpool, item->key_partial.data);
            }
            ngx_slab_free(queue->shpool, item);
            item = NULL;
            continue;  /* try next head */
        }

        break;  /* got a live item */
    }

    queue_remaining = (ngx_uint_t) queue->size;
    ngx_shmtx_unlock(&queue->mutex);

    if (item == NULL) {
        return NGX_OK;   /* queue empty */
    }

    /* Walk the cache directory outside the lock — no blocking sleep */
    ngx_memzero(&ctx,  sizeof(ngx_http_cache_purge_walk_ctx_t));
    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));

    tree.pre_tree_handler  = ngx_http_purge_file_cache_noop;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler      = ngx_http_purge_file_cache_noop;
    tree.data              = &ctx;
    tree.log               = cycle->log;

    if (item->purge_all) {
        tree.file_handler = ngx_http_purge_file_cache_delete_file;
        ngx_walk_tree(&tree, &item->cache_path);

    } else if (item->key_partial.len > 0) {
        ctx.key_partial = item->key_partial.data;
        ctx.key_len     = item->key_partial.len;
        if (ctx.key_len > 0 && ctx.key_partial[ctx.key_len - 1] == '*') {
            ctx.key_len--;
        }
        tree.file_handler = ngx_http_purge_file_cache_delete_partial_file;
        ngx_walk_tree(&tree, &item->cache_path);
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, cycle->log, 0,
                   "ngx_cache_purge: background walk of \"%V\" key \"%V\" "
                   "deleted %ui file(s)",
                   &item->cache_path, &item->key_partial, ctx.files_deleted);

    ngx_slab_free(queue->shpool, item->cache_path.data);
    if (item->key_partial.data) {
        ngx_slab_free(queue->shpool, item->key_partial.data);
    }
    ngx_slab_free(queue->shpool, item);

    return (queue_remaining > 0) ? NGX_AGAIN : NGX_OK;
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
    ngx_uint_t hash, ngx_str_t *cache_path, ngx_str_t *key)
{
    ngx_http_cache_purge_queue_item_t *item;

    for (item = queue->head; item; item = item->next) {
        /*
         * Two-step check: hash first (fast), then full string comparison.
         * Comparing only the hash is insufficient because a 32-bit
         * multiplier hash will collide for distinct keys in large caches,
         * causing legitimate purge requests to be silently discarded.
         */
        if (item->hash != hash) {
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

    if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0) {
        cmcf->vary_aware = 1;
    } else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0) {
        cmcf->vary_aware = 0;
    } else {
        return "invalid value, use 'on' or 'off'";
    }

    return NGX_CONF_OK;
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
    ngx_http_cache_purge_walk_ctx_t *wctx = ctx->data;
    ngx_file_t                       file;
    ngx_flag_t                       remove_file = 0;
    ngx_int_t                        n;

    wctx->files_checked++;

    if (wctx->key_len == 0) {
        /* stripped wildcard — match everything */
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
                          sizeof(ngx_http_file_cache_header_t) + 6);
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
 * Exact-match walk handler (vary-aware).
 *
 * Reads (key_len + 1) bytes from the KEY: region of each cache file.
 * The file is deleted only when:
 *   - the first key_len bytes match key_partial exactly, AND
 *   - the byte at position key_len is '\n'
 *
 * The '\n' check confirms the stored key is exactly key_len characters long,
 * preventing false matches against keys that share a common prefix.
 *
 * Because all Vary variants of a cached response store the same KEY: string,
 * this walk removes every variant regardless of its filesystem path.
 *
 * The primary file was already deleted by ngx_http_file_cache_purge().  If
 * that path appears again during the walk, ngx_delete_file will return ENOENT;
 * this is silently ignored rather than logged as a critical error.
 */
static ngx_int_t
ngx_http_purge_file_cache_delete_exact_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path)
{
    ngx_http_cache_purge_walk_ctx_t *wctx = ctx->data;
    ngx_file_t                       file;
    ngx_int_t                        n;

    wctx->files_checked++;

    /* key_len == 0 or buffer too small to hold key + terminator: skip */
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

    /* Read key_len + 1: the key string plus its terminating '\n' */
    n = ngx_read_file(&file, wctx->key_buffer, wctx->key_len + 1,
                      sizeof(ngx_http_file_cache_header_t) + 6);
    ngx_close_file(file.fd);

    if (n != (ngx_int_t)(wctx->key_len + 1)) {
        return NGX_OK;
    }

    if (wctx->key_buffer[wctx->key_len] != '\n') {
        /* Stored key is longer than ours — not an exact match */
        return NGX_OK;
    }

    if (ngx_strncasecmp(wctx->key_buffer, wctx->key_partial,
                        wctx->key_len) != 0)
    {
        return NGX_OK;
    }

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        /* Silently ignore ENOENT: primary file was already deleted */
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

    tree.file_handler      = ngx_http_purge_file_cache_delete_exact_file;
    tree.pre_tree_handler  = ngx_http_purge_file_cache_noop;
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
    ngx_http_cache_purge_walk_ctx_t *wctx = ctx->data;

    if (wctx != NULL) {
        wctx->files_deleted++;
    }

    if (ngx_delete_file(path->data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, ctx->log, ngx_errno,
                      "ngx_cache_purge: could not delete \"%V\"", path);
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

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->fastcgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
        if (ngx_http_cache_purge_conf(cf, &cplcf->fastcgi) != NGX_CONF_OK) {
            return NGX_CONF_ERROR;
        }

        if (cplcf->fastcgi.refresh) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"refresh\" is supported only with \"proxy_cache_purge\"");
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
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
        /* purge_all empties the zone — always report 200 regardless of
         * how many files existed.  Skip ngx_http_cache_purge_handler()
         * so we never attempt an exact-key lookup on a bulk operation. */
        r->main->count++;
        ngx_http_finalize_request(r,
            ngx_http_cache_purge_send_response(r, &status));
        return NGX_DONE;
    }

    if (ngx_http_cache_purge_is_partial(r)) {
        ngx_uint_t deleted = ngx_http_cache_purge_partial(r, cache);
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
    /* FIX (#52): nginx 1.29.4 inserted host_set here — without this guard
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
        /* dynamic zone expression — allocate a persistent copy */
        cplcf->proxy_separate_value = ngx_palloc(cf->pool,
                                          sizeof(ngx_http_complex_value_t));
        if (cplcf->proxy_separate_value == NULL) {
            return NGX_CONF_ERROR;
        }
        *cplcf->proxy_separate_value = cv;
    } else {
        /* static zone name — look it up in shared memory table */
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
#  if (nginx_version >= 1007009)
    ngx_http_proxy_main_conf_t       *pmcf;
    ngx_int_t                         rc;
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
        ngx_str_t  cv_val;

        if (cplcf->proxy_separate_zone) {
            cache = cplcf->proxy_separate_zone->data;
        } else {
            /* dynamic zone name — evaluate and walk the proxy caches list */
            ngx_uint_t              i;
            ngx_str_t              *name;
            ngx_http_file_cache_t **caches;

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

    if (cplcf->conf->refresh) {
        return ngx_http_cache_purge_refresh(r, cache);
    }

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
        ngx_uint_t deleted = ngx_http_cache_purge_partial(r, cache);
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

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->scgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
        if (ngx_http_cache_purge_conf(cf, &cplcf->scgi) != NGX_CONF_OK) {
            return NGX_CONF_ERROR;
        }

        if (cplcf->scgi.refresh) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"refresh\" is supported only with \"proxy_cache_purge\"");
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
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
        ngx_uint_t deleted = ngx_http_cache_purge_partial(r, cache);
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

    cplcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_cache_purge_module);

    if (cplcf->uwsgi.enable != NGX_CONF_UNSET) {
        return "is duplicate";
    }

    if (cf->args->nelts != 3) {
        if (ngx_http_cache_purge_conf(cf, &cplcf->uwsgi) != NGX_CONF_OK) {
            return NGX_CONF_ERROR;
        }

        if (cplcf->uwsgi.refresh) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"refresh\" is supported only with \"proxy_cache_purge\"");
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
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
        ngx_uint_t deleted = ngx_http_cache_purge_partial(r, cache);
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


/* -- response type directive -------------------------------------------- */

char *
ngx_http_cache_purge_response_type_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_cache_purge_loc_conf_t *cplcf = conf;
    ngx_str_t                       *value;

    if (cplcf->resptype != NGX_CONF_UNSET_UINT
        && cf->cmd_type == NGX_HTTP_LOC_CONF)
    {
        return "is duplicate";
    }

    if (cf->args->nelts != 2) {
        return "requires exactly one argument: html|json|xml|text";
    }

    if (cf->cmd_type == NGX_HTTP_MODULE) {
        return "(separate server or location syntax) is not allowed here";
    }

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "html") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_HTML;
    } else if (ngx_strcmp(value[1].data, "json") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_JSON;
    } else if (ngx_strcmp(value[1].data, "xml") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_XML;
    } else if (ngx_strcmp(value[1].data, "text") == 0) {
        cplcf->resptype = NGX_REPONSE_TYPE_TEXT;
    } else {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid parameter \"%V\", expected html|json|xml|text",
            &value[1]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/* -- access control ----------------------------------------------------- */

static ngx_int_t
ngx_http_cache_purge_invalidate_file(ngx_tree_ctx_t *ctx, ngx_str_t *path)
{
    ngx_http_cache_purge_batch_ctx_t         *data;
    ngx_http_cache_purge_invalidate_item_t    item;
    ngx_http_cache_purge_invalidate_result_e  result;
    ngx_pool_t                               *pool;

    data = ctx->data;
    pool = ngx_create_pool(4096, ctx->log);
    if (pool == NULL) {
        return NGX_OK;
    }

    if (ngx_http_cache_purge_read_item(pool, ctx->log, path, &item) == NGX_OK) {
        if (ngx_http_cache_purge_invalidate_item(data->request, data->cache,
                                                 pool, &item, &result)
            != NGX_OK)
        {
            ngx_log_error(NGX_LOG_CRIT, ctx->log, 0,
                          "http file cache invalidate failed for \"%V\"",
                          path);
        } else if (result == NGX_HTTP_CACHE_PURGE_INVALIDATE_PURGED) {
            data->files_deleted++;
        }
    }

    if (ngx_http_cache_purge_enqueue_temp_pool(&data->temp_pools,
                                               data->request->pool, pool)
        != NGX_OK)
    {
        ngx_destroy_pool(pool);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_invalidate_partial_file(ngx_tree_ctx_t *ctx,
    ngx_str_t *path)
{
    ngx_http_cache_purge_batch_ctx_t         *data;
    ngx_http_cache_purge_invalidate_item_t    item;
    ngx_http_cache_purge_invalidate_result_e  result;
    ngx_pool_t                               *pool;

    data = ctx->data;
    pool = ngx_create_pool(4096, ctx->log);
    if (pool == NULL) {
        return NGX_OK;
    }

    if (ngx_http_cache_purge_read_item(pool, ctx->log, path, &item) == NGX_OK) {
        if (data->partial_prefix.len == 0
            || (item.cache_key.len >= data->partial_prefix.len
                && ngx_strncmp(item.cache_key.data,
                               data->partial_prefix.data,
                               data->partial_prefix.len) == 0))
        {
            if (ngx_http_cache_purge_invalidate_item(data->request, data->cache,
                                                     pool, &item, &result)
                != NGX_OK)
            {
                ngx_log_error(NGX_LOG_CRIT, ctx->log, 0,
                              "http partial cache invalidate failed for \"%V\"",
                              path);
            } else if (result == NGX_HTTP_CACHE_PURGE_INVALIDATE_PURGED) {
                data->files_deleted++;
            }
        }
    }

    if (ngx_http_cache_purge_enqueue_temp_pool(&data->temp_pools,
                                               data->request->pool, pool)
        != NGX_OK)
    {
        ngx_destroy_pool(pool);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_enqueue_temp_pool(ngx_queue_t *queue,
    ngx_pool_t *owner_pool, ngx_pool_t *pool)
{
    ngx_http_cache_purge_refresh_temp_pool_t  *entry;

    if (pool == NULL) {
        return NGX_OK;
    }

    entry = ngx_palloc(owner_pool,
                       sizeof(ngx_http_cache_purge_refresh_temp_pool_t));
    if (entry == NULL) {
        return NGX_ERROR;
    }

    entry->pool = pool;
    ngx_queue_insert_tail(queue, &entry->queue);

    return NGX_OK;
}


static void
ngx_http_cache_purge_drain_temp_pools(ngx_queue_t *queue)
{
    ngx_queue_t                              *q;
    ngx_http_cache_purge_refresh_temp_pool_t *entry;

    while (!ngx_queue_empty(queue)) {
        q = ngx_queue_head(queue);
        entry = (ngx_http_cache_purge_refresh_temp_pool_t *) ngx_queue_data(
                    q, ngx_http_cache_purge_refresh_temp_pool_t, queue);
        ngx_queue_remove(q);

        if (entry->pool != NULL) {
            ngx_destroy_pool(entry->pool);
        }
    }
}


static ngx_int_t
ngx_http_cache_purge_read_item(ngx_pool_t *pool, ngx_log_t *log,
    ngx_str_t *path, ngx_http_cache_purge_invalidate_item_t *item)
{
    ngx_http_file_cache_header_t  header;
    ngx_file_t                    file;
    ngx_file_info_t               fi;
    u_char                       *key_buf;
    size_t                        key_len;
    ssize_t                       n;

    ngx_memzero(item, sizeof(ngx_http_cache_purge_invalidate_item_t));
    ngx_memzero(&file, sizeof(ngx_file_t));

    file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN,
                            NGX_FILE_DEFAULT_ACCESS);
    if (file.fd == NGX_INVALID_FILE) {
        return NGX_ERROR;
    }

    file.name = *path;
    file.log = log;

    if (ngx_fd_info(file.fd, &fi) == NGX_FILE_ERROR) {
        ngx_close_file(file.fd);
        return NGX_ERROR;
    }

    item->fs_size = ngx_file_size(&fi);
    item->uniq = ngx_file_uniq(&fi);

    n = ngx_read_file(&file, (u_char *) &header, sizeof(header), 0);
    if (n < (ssize_t) sizeof(header)) {
        ngx_close_file(file.fd);
        return NGX_ERROR;
    }

    if (header.header_start <= sizeof(header) + 6) {
        ngx_close_file(file.fd);
        return NGX_ERROR;
    }

    key_len = header.header_start - sizeof(header) - 6;
    if (key_len == 0 || key_len > 8192) {
        ngx_close_file(file.fd);
        return NGX_ERROR;
    }

    key_buf = ngx_pnalloc(pool, key_len + 1);
    if (key_buf == NULL) {
        ngx_close_file(file.fd);
        return NGX_ERROR;
    }

    n = ngx_read_file(&file, key_buf, key_len, sizeof(header) + 6);
    ngx_close_file(file.fd);
    if (n < 1) {
        return NGX_ERROR;
    }

    key_buf[n] = '\0';
    if (n > 0 && key_buf[n - 1] == LF) {
        key_buf[n - 1] = '\0';
        n--;
    }

    item->cache_key.data = key_buf;
    item->cache_key.len = n;

    item->cache_path.data = ngx_pnalloc(pool, path->len + 1);
    if (item->cache_path.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(item->cache_path.data, path->data, path->len);
    item->cache_path.data[path->len] = '\0';
    item->cache_path.len = path->len;

    item->last_modified = header.last_modified;
    item->body_start = header.body_start;
    if (header.etag_len > 0 && header.etag_len < NGX_HTTP_CACHE_ETAG_LEN) {
        item->etag_len = header.etag_len;
        ngx_memcpy(item->etag, header.etag, header.etag_len);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_invalidate_opened_cache(ngx_log_t *log,
    ngx_http_file_cache_t *cache, ngx_http_cache_t *c,
    ngx_pool_t *pool, ngx_http_cache_purge_invalidate_item_t *item,
    ngx_http_cache_purge_invalidate_result_e *result)
{
    (void) pool;

    ngx_shmtx_lock(&cache->shpool->mutex);

    if (!c->node->exists) {
        ngx_shmtx_unlock(&cache->shpool->mutex);
        *result = NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_MISSING;
        return NGX_OK;
    }

    if (item != NULL) {
        if (!ngx_http_cache_purge_item_matches_cache(item, c)
            || !ngx_http_cache_purge_cache_matches_node(c))
        {
            ngx_shmtx_unlock(&cache->shpool->mutex);
            *result = NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_REPLACED;
            return NGX_OK;
        }
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
        ngx_log_error(NGX_LOG_CRIT, log, ngx_errno,
                      ngx_delete_file_n " \"%s\" failed", c->file.name.data);
    }

    *result = NGX_HTTP_CACHE_PURGE_INVALIDATE_PURGED;
    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_open_temp_cache(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_pool_t *pool, ngx_str_t *cache_key,
    ngx_http_cache_t *c)
{
    ngx_http_request_t  tr;
    ngx_str_t          *key;

    ngx_memzero(c, sizeof(ngx_http_cache_t));

    /*
     * Shallow-copy the request struct so we can override pool and cache
     * without mutating the original.  ngx_http_file_cache_create_key and
     * ngx_http_file_cache_open read r->cache, r->pool, r->connection->log,
     * and loc_conf pointers — all of which remain valid through the shallow
     * copy.  This is fragile if nginx internals change; keep the scope of
     * tr usage minimal and do not pass &tr to anything beyond these two
     * cache API calls.
     */
    ngx_memcpy(&tr, r, sizeof(ngx_http_request_t));

    tr.pool = pool;
    tr.cache = c;

    if (ngx_array_init(&c->keys, pool, 1, sizeof(ngx_str_t)) != NGX_OK) {
        return NGX_ERROR;
    }

    key = ngx_array_push(&c->keys);
    if (key == NULL) {
        return NGX_ERROR;
    }

    *key = *cache_key;
    c->body_start = ngx_pagesize;
    c->file_cache = cache;
    c->file.log = r->connection->log;

    ngx_http_file_cache_create_key(&tr);

    switch (ngx_http_file_cache_open(&tr)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
#  if (nginx_version >= 8001) \
       || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
#  endif
        return NGX_OK;

    case NGX_DECLINED:
        return NGX_DECLINED;

    default:
        return NGX_ERROR;
    }
}


static ngx_int_t
ngx_http_cache_purge_item_matches_cache(
    ngx_http_cache_purge_invalidate_item_t *expected,
    ngx_http_cache_t *c)
{
    if (expected->etag_len > 0) {
        if (expected->etag_len != c->etag.len
            || c->etag.data == NULL
            || ngx_memcmp(expected->etag, c->etag.data, expected->etag_len) != 0)
        {
            return 0;
        }
    }

    if (expected->last_modified > 0) {
        if (expected->last_modified != c->last_modified) {
            return 0;
        }
    }

    if (expected->uniq != 0) {
        if (expected->uniq != c->uniq) {
            return 0;
        }
    }

    if (expected->body_start > 0) {
        if (expected->body_start != c->body_start) {
            return 0;
        }
    }

    /*
     * If no fields were available for comparison (all zero/empty in the
     * scanned item), we cannot confirm a race.  Allow the invalidation
     * rather than silently skipping it — a false positive purge is safer
     * than a false negative that leaves stale content.
     */
    return 1;
}


static ngx_int_t
ngx_http_cache_purge_cache_matches_node(ngx_http_cache_t *c)
{
    if (!c->node->exists) {
        return 0;
    }

    if (c->uniq != 0 && c->node->uniq != c->uniq) {
        return 0;
    }

#  if (nginx_version >= 1000001)
    if (c->node->fs_size != c->fs_size) {
        return 0;
    }
#  endif

    if (c->node->body_start != c->body_start) {
        return 0;
    }

    return 1;
}


static ngx_int_t
ngx_http_cache_purge_invalidate_item(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_pool_t *pool,
    ngx_http_cache_purge_invalidate_item_t *item,
    ngx_http_cache_purge_invalidate_result_e *result)
{
    ngx_http_cache_t                     *c;
    ngx_int_t                              rc;

    *result = NGX_HTTP_CACHE_PURGE_INVALIDATE_ERROR;

    /*
     * Allocate c on the pool rather than the stack.
     * ngx_http_file_cache_open registers a pool cleanup that references c,
     * so c must survive until the pool is destroyed.
     */
    c = ngx_pcalloc(pool, sizeof(ngx_http_cache_t));
    if (c == NULL) {
        return NGX_ERROR;
    }

    rc = ngx_http_cache_purge_open_temp_cache(r, cache, pool, &item->cache_key, c);
    if (rc == NGX_DECLINED) {
        *result = NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_MISSING;
        return NGX_OK;
    }

    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    rc = ngx_http_cache_purge_invalidate_opened_cache(r->connection->log,
                                                      cache, c, NULL, item,
                                                      result);

    /*
     * Release the node reference acquired by open_temp_cache.
     * ngx_http_file_cache_free decrements count under mutex and,
     * if exists==0 && count==0, safely removes the node from the
     * rbtree and frees the slab memory.  It also sets c->updated=1,
     * so the pool cleanup (ngx_http_file_cache_cleanup) will be a
     * no-op when the pool is eventually destroyed.
     */
    ngx_http_file_cache_free(c, NULL);

    return rc;
}

ngx_int_t
ngx_http_cache_purge_access_handler(ngx_http_request_t *r)
{
    ngx_http_cache_purge_loc_conf_t *cplcf;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

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
    size_t                           body_len, resp_tmpl_len, len;
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

    switch (cplcf->resptype) {
    case NGX_REPONSE_TYPE_JSON:
        resp_ct        = ngx_http_cache_purge_content_type_json;
        resp_ct_size   = ngx_http_cache_purge_content_type_json_size;
        resp_body      = ngx_http_cache_purge_body_templ_json;
        resp_body_size = ngx_http_cache_purge_body_templ_json_size;
        break;
    case NGX_REPONSE_TYPE_XML:
        resp_ct        = ngx_http_cache_purge_content_type_xml;
        resp_ct_size   = ngx_http_cache_purge_content_type_xml_size;
        resp_body      = ngx_http_cache_purge_body_templ_xml;
        resp_body_size = ngx_http_cache_purge_body_templ_xml_size;
        break;
    case NGX_REPONSE_TYPE_TEXT:
        resp_ct        = ngx_http_cache_purge_content_type_text;
        resp_ct_size   = ngx_http_cache_purge_content_type_text_size;
        resp_body      = ngx_http_cache_purge_body_templ_text;
        resp_body_size = ngx_http_cache_purge_body_templ_text_size;
        break;
    default:
    case NGX_REPONSE_TYPE_HTML:
        resp_ct        = ngx_http_cache_purge_content_type_html;
        resp_ct_size   = ngx_http_cache_purge_content_type_html_size;
        resp_body      = ngx_http_cache_purge_body_templ_html;
        resp_body_size = ngx_http_cache_purge_body_templ_html_size;
        break;
    }

    body_len      = resp_body_size - 2 - 2 - 1;
    resp_tmpl_len = body_len + key[0].len + status->len;
    len           = resp_tmpl_len;

    r->headers_out.content_type.len  = resp_ct_size - 1;
    r->headers_out.content_type.data = (u_char *) resp_ct;

    buf = ngx_pcalloc(r->pool, resp_tmpl_len + 1);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* ngx_snprintf never returns NULL */
    ngx_snprintf(buf, resp_tmpl_len, resp_body, buf_keydata, status->data);

    r->headers_out.status           = (r->headers_out.status == NGX_HTTP_ACCEPTED)
                                      ? NGX_HTTP_ACCEPTED : NGX_HTTP_OK;
    r->headers_out.content_length_n = (off_t) len;

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf  = b;
    out.next = NULL;

    b->last     = ngx_cpymem(b->last, buf, len);
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
    ngx_http_cache_purge_invalidate_result_e  result;

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

    if (ngx_http_cache_purge_invalidate_opened_cache(r->connection->log,
                                                     cache, c, NULL, NULL,
                                                     &result)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    if (result == NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_MISSING) {
        return NGX_DECLINED;
    }
    return NGX_OK;
}


/* -- bulk walk helpers -------------------------------------------------- */

void
ngx_http_cache_purge_all(ngx_http_request_t *r, ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_batch_ctx_t  ctx;
    ngx_tree_ctx_t                    tree;

    ngx_memzero(&ctx, sizeof(ngx_http_cache_purge_batch_ctx_t));
    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "purge_all http in %s",
                  cache->path->name.data);

    ctx.request   = r;
    ctx.cache     = cache;
    ctx.match_all = 1;
    ctx.files_deleted = 0;
    ngx_queue_init(&ctx.temp_pools);

    tree.file_handler      = ngx_http_cache_purge_invalidate_file;
    tree.pre_tree_handler  = ngx_http_purge_file_cache_noop;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler      = ngx_http_purge_file_cache_noop;
    tree.data              = &ctx;
    tree.log               = ngx_cycle->log;

    ngx_walk_tree(&tree, &cache->path->name);
    ngx_http_cache_purge_drain_temp_pools(&ctx.temp_pools);
}

ngx_uint_t
ngx_http_cache_purge_partial(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_batch_ctx_t  ctx;
    ngx_tree_ctx_t                    tree;
    ngx_str_t                        *key;
    ngx_uint_t                        len;

    ngx_memzero(&ctx, sizeof(ngx_http_cache_purge_batch_ctx_t));
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "purge_partial http in %s",
                  cache->path->name.data);

    key = r->cache->keys.elts;
    len = key[0].len;

    if (len > 0 && key[0].data[len - 1] == '*') {
        len--;
    }

    ngx_memzero(&ctx, sizeof(ngx_http_cache_purge_batch_ctx_t));
    ngx_memzero(&tree, sizeof(ngx_tree_ctx_t));

    ctx.request = r;
    ctx.cache   = cache;
    ctx.partial_prefix.data = key[0].data;
    ctx.partial_prefix.len  = len;
    ctx.files_deleted = 0;
    ngx_queue_init(&ctx.temp_pools);
    ctx.match_all = (len == 0);

    tree.file_handler      = ngx_http_cache_purge_invalidate_partial_file;
    tree.pre_tree_handler  = ngx_http_purge_file_cache_noop;
    tree.post_tree_handler = ngx_http_purge_file_cache_noop;
    tree.spec_handler      = ngx_http_purge_file_cache_noop;
    tree.data              = &ctx;
    tree.log               = ngx_cycle->log;

    ngx_walk_tree(&tree, &cache->path->name);
    return ctx.files_deleted;
    ngx_http_cache_purge_drain_temp_pools(&ctx.temp_pools);

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

    /* We will refresh (validate) instead of blindly purging */
    if (from_position < cf->args->nelts
        && ngx_strcmp(value[from_position].data, "refresh") == 0)
    {
        cpcf->refresh = 1;
        from_position++;
    }


    if (from_position >= cf->args->nelts) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "missing \"from\" keyword after optional parameters");
        return NGX_CONF_ERROR;
    }

    /* sanity check */
    if (ngx_strcmp(value[from_position].data, "from") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid parameter \"%V\", expected \"from\" keyword",
            &value[from_position]);
        return NGX_CONF_ERROR;
    }

    if (from_position + 1 >= cf->args->nelts) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "missing argument after \"from\" keyword");
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
            conf->refresh = prev->refresh;
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

    conf->resptype = NGX_CONF_UNSET_UINT;
    conf->refresh_concurrency = NGX_CONF_UNSET_UINT;
    conf->refresh_timeout = NGX_CONF_UNSET_MSEC;

    conf->conf = NGX_CONF_UNSET_PTR;

    return conf;
}

char *
ngx_http_cache_purge_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_cache_purge_loc_conf_t *prev = parent;
    ngx_http_cache_purge_loc_conf_t *conf = child;
    ngx_http_core_loc_conf_t        *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    ngx_conf_merge_uint_value(conf->resptype, prev->resptype,
                              NGX_REPONSE_TYPE_HTML);

    ngx_conf_merge_uint_value(conf->refresh_concurrency,
                              prev->refresh_concurrency, 32);
    ngx_conf_merge_msec_value(conf->refresh_timeout,
                              prev->refresh_timeout, 30000);

# if (NGX_HTTP_FASTCGI)
    ngx_http_cache_purge_merge_conf(&conf->fastcgi, &prev->fastcgi);

    if (conf->fastcgi.enable) {
        conf->conf             = &conf->fastcgi;
        conf->handler          = ngx_http_fastcgi_cache_purge_handler;
        conf->original_handler = clcf->handler;  /* may be NULL */
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

# if (NGX_HTTP_PROXY)
    ngx_http_cache_purge_merge_conf(&conf->proxy, &prev->proxy);

    if (conf->proxy.enable) {
        /*
         * Install the purge access-handler even when clcf->handler is
         * NULL (i.e. when proxy_cache is configured without proxy_pass).
         * original_handler may legitimately be NULL here; the access
         * handler guards against dereferencing it below.
         */
        conf->conf             = &conf->proxy;
        conf->handler          = ngx_http_proxy_cache_purge_handler;
        conf->original_handler = clcf->handler;  /* may be NULL */
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

# if (NGX_HTTP_SCGI)
    ngx_http_cache_purge_merge_conf(&conf->scgi, &prev->scgi);

    if (conf->scgi.enable) {
        conf->conf             = &conf->scgi;
        conf->handler          = ngx_http_scgi_cache_purge_handler;
        conf->original_handler = clcf->handler;  /* may be NULL */
        clcf->handler          = ngx_http_cache_purge_access_handler;
        return NGX_CONF_OK;
    }
# endif

# if (NGX_HTTP_UWSGI)
    ngx_http_cache_purge_merge_conf(&conf->uwsgi, &prev->uwsgi);

    if (conf->uwsgi.enable) {
        conf->conf             = &conf->uwsgi;
        conf->handler          = ngx_http_uwsgi_cache_purge_handler;
        conf->original_handler = clcf->handler;  /* may be NULL */
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


/*
 * ===================== Refresh Feature Implementation =====================
 */

static ngx_str_t  ngx_http_cache_purge_refresh_bypass_name =
    ngx_string("cache_purge_refresh_bypass");

static ngx_str_t  ngx_http_head_method_name = ngx_string("HEAD");


/*
 * Variable handler for $cache_purge_refresh_bypass.
 * Returns "1" when the current request is a refresh subrequest,
 * empty otherwise. Used with proxy_cache_bypass and proxy_no_cache.
 */
static ngx_int_t
ngx_http_cache_purge_refresh_bypass_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_cache_purge_refresh_ctx_t  *ctx;

    if (r->parent != NULL) {
        ctx = ngx_http_get_module_ctx(r->parent,
                                      ngx_http_cache_purge_module);
        if (ctx != NULL) {
            v->len = 1;
            v->valid = 1;
            v->no_cacheable = 1;
            v->not_found = 0;
            v->data = (u_char *) "1";
            return NGX_OK;
        }
    }

    v->not_found = 1;
    return NGX_OK;
}


/*
 * Register the $cache_purge_refresh_bypass variable during preconfiguration.
 */
static ngx_int_t
ngx_http_cache_purge_add_variable(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf,
                                &ngx_http_cache_purge_refresh_bypass_name,
                                NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_cache_purge_refresh_bypass_variable;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_refresh_collect_open_file(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    switch (ngx_http_file_cache_open(r)) {
    case NGX_OK:
    case NGX_HTTP_CACHE_STALE:
#  if (nginx_version >= 8001) \
       || ((nginx_version < 8000) && (nginx_version >= 7060))
    case NGX_HTTP_CACHE_UPDATING:
#  endif
        return ngx_http_cache_purge_refresh_collect_path(ctx,
                                                         &r->cache->file.name,
                                                         1);

    case NGX_DECLINED:
        return NGX_OK;

#  if (NGX_HAVE_FILE_AIO)
    case NGX_AGAIN:
#  endif
    default:
        return NGX_ERROR;
    }
}


static ngx_int_t
ngx_http_cache_purge_refresh_collect_path(
    ngx_http_cache_purge_refresh_ctx_t *rctx, ngx_str_t *path,
    ngx_uint_t exact_match)
{
    ngx_http_cache_purge_refresh_file_t *file;
    ngx_http_file_cache_header_t         header;
    ngx_file_t                           f;
    ngx_file_info_t                      fi;
    ngx_str_t                            path_copy, uri, args, etag, cache_key;
    ngx_http_cache_purge_invalidate_item_t item;
    u_char                              *key_buf;
    u_char                              *path_data;
    u_char                              *uri_data;
    u_char                              *args_data;
    u_char                              *etag_data;
    u_char                              *cache_key_data;
    ssize_t                              n;
    size_t                               key_read_len;
    u_char                              *p, *q;
    ngx_pool_t                          *pool;

    ngx_memzero(&path_copy, sizeof(ngx_str_t));
    ngx_memzero(&uri, sizeof(ngx_str_t));
    ngx_memzero(&args, sizeof(ngx_str_t));
    ngx_memzero(&etag, sizeof(ngx_str_t));
    ngx_memzero(&cache_key, sizeof(ngx_str_t));
    ngx_memzero(&item, sizeof(ngx_http_cache_purge_invalidate_item_t));

    pool = rctx->chunk_pool != NULL ? rctx->chunk_pool : rctx->request->pool;

    if (rctx->timeout_enabled && !rctx->timed_out
        && ngx_current_msec >= rctx->deadline)
    {
        ngx_http_cache_purge_refresh_mark_timeout(rctx);
        return NGX_ABORT;
    }

    /* Open cache file */
    ngx_memzero(&f, sizeof(ngx_file_t));
    f.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN,
                         NGX_FILE_DEFAULT_ACCESS);
    if (f.fd == NGX_INVALID_FILE) {
        return NGX_OK;  /* skip unreadable files */
    }
    f.name.data = path->data;
    f.name.len = path->len;
    f.log = ngx_cycle->log;

    if (ngx_fd_info(f.fd, &fi) == NGX_FILE_ERROR) {
        ngx_close_file(f.fd);
        return NGX_OK;
    }

    /* Read binary header */
    n = ngx_read_file(&f, (u_char *) &header, sizeof(header), 0);
    if (n < (ssize_t) sizeof(header)) {
        ngx_close_file(f.fd);
        return NGX_OK;  /* skip corrupt/truncated files */
    }

    /*
     * Read the cache key from file.
     * Layout: [header][\nKEY: ][key_data][\n][HTTP headers...]
     * We need to read enough to get the full key. Use header_start as
     * upper bound (key ends before HTTP headers start).
     */
    if (header.header_start <= sizeof(header) + 6) {
        ngx_close_file(f.fd);
        return NGX_OK;
    }

    key_read_len = header.header_start - sizeof(header) - 6;
    if (key_read_len == 0 || key_read_len > 8192) {
        ngx_close_file(f.fd);
        return NGX_OK;  /* invalid or too long */
    }

    key_buf = ngx_alloc(key_read_len + 1, ngx_cycle->log);
    if (key_buf == NULL) {
        ngx_close_file(f.fd);
        return NGX_ERROR;
    }

    n = ngx_read_file(&f, key_buf, key_read_len,
                      sizeof(header) + 6);  /* skip header + "\nKEY: " */
    ngx_close_file(f.fd);

    if (n < 1) {
        ngx_free(key_buf);
        return NGX_OK;
    }

    /* Null-terminate and strip trailing LF */
    key_buf[n] = '\0';
    if (n > 0 && key_buf[n - 1] == LF) {
        key_buf[n - 1] = '\0';
        n--;
    }

    if (exact_match) {
        if ((size_t) n != rctx->key_partial.len
            || ngx_strncmp(key_buf, rctx->key_partial.data,
                           rctx->key_partial.len) != 0)
        {
            ngx_free(key_buf);
            return NGX_OK;
        }

    } else if (!rctx->purge_all && rctx->key_partial.len > 0) {
        /* Check if key matches our partial prefix */
        if ((size_t) n < rctx->key_partial.len) {
            ngx_free(key_buf);
            return NGX_OK;  /* key too short to match */
        }
        if (ngx_strncmp(key_buf, rctx->key_partial.data,
                        rctx->key_partial.len) != 0)
        {
            ngx_free(key_buf);
            return NGX_OK;  /* no match */
        }
    }

    if ((size_t) n < rctx->key_prefix_len) {
        ngx_free(key_buf);
        return NGX_OK;
    }

    path_data = ngx_pnalloc(pool, path->len + 1);
    if (path_data == NULL) {
        ngx_free(key_buf);
        return NGX_ERROR;
    }
    ngx_memcpy(path_data, path->data, path->len);
    path_data[path->len] = '\0';
    path_copy.len = path->len;
    path_copy.data = path_data;
    item.cache_path = path_copy;

    /* Extract URI from key by removing the non-URI prefix */
    p = key_buf + rctx->key_prefix_len;

    /* Split URI and args at '?' */
    q = (u_char *) ngx_strchr(p, '?');
    if (q != NULL) {
        uri.len = q - p;
        uri_data = ngx_pnalloc(pool, uri.len + 1);
        if (uri_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(uri_data, p, uri.len);
        uri_data[uri.len] = '\0';
        uri.data = uri_data;

        q++;  /* skip '?' */
        if ((size_t) n < rctx->key_prefix_len + uri.len + 1) {
            ngx_free(key_buf);
            return NGX_OK;  /* malformed key, skip */
        }
        args.len = n - rctx->key_prefix_len - uri.len - 1;
        args_data = ngx_pnalloc(pool, args.len + 1);
        if (args_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(args_data, q, args.len);
        args_data[args.len] = '\0';
        args.data = args_data;
    } else {
        uri.len = n - rctx->key_prefix_len;
        uri_data = ngx_pnalloc(pool, uri.len + 1);
        if (uri_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(uri_data, p, uri.len);
        uri_data[uri.len] = '\0';
        uri.data = uri_data;
    }

    /* Store ETag from binary header */
    if (header.etag_len > 0 && header.etag_len < NGX_HTTP_CACHE_ETAG_LEN) {
        etag_data = ngx_pnalloc(pool, header.etag_len + 1);
        if (etag_data == NULL) {
            ngx_free(key_buf);
            return NGX_ERROR;
        }
        ngx_memcpy(etag_data, header.etag, header.etag_len);
        etag_data[header.etag_len] = '\0';
        etag.len = header.etag_len;
        etag.data = etag_data;
    }

    item.etag_len = etag.len;
    if (etag.len > 0) {
        ngx_memcpy(item.etag, etag.data, etag.len);
    }

    /* Store Last-Modified */
    item.uniq = ngx_file_uniq(&fi);
    item.last_modified = header.last_modified;
    item.body_start = header.body_start;
    item.fs_size = ngx_file_size(&fi);

    cache_key_data = ngx_pnalloc(pool, n + 1);
    if (cache_key_data == NULL) {
        ngx_free(key_buf);
        return NGX_ERROR;
    }
    ngx_memcpy(cache_key_data, key_buf, n);
    cache_key_data[n] = '\0';
    cache_key.len = n;
    cache_key.data = cache_key_data;
    item.cache_key = cache_key;

    ngx_free(key_buf);

    file = ngx_array_push(rctx->files);
    if (file == NULL) {
        return NGX_ERROR;
    }

    file->path = path_copy;
    file->uri = uri;
    file->args = args;
    file->etag = etag;
    file->last_modified = header.last_modified;
    file->item = item;

    rctx->queued++;
    rctx->total++;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "refresh collect: uri=\"%V\" etag=\"%V\" path=\"%V\"",
                   &file->uri, &file->etag, &file->path);

    return NGX_OK;
}


/*
 * Subrequest post-handler: called when a refresh HEAD subrequest completes.
 * Checks upstream status to decide whether to keep or purge the cache file.
 */
static ngx_int_t
ngx_http_cache_purge_refresh_done(ngx_http_request_t *r, void *data,
    ngx_int_t rc)
{
    ngx_http_cache_purge_refresh_post_data_t *pd;
    ngx_http_cache_purge_refresh_ctx_t       *ctx;
    ngx_http_cache_purge_refresh_file_t      *file;
    ngx_http_cache_purge_invalidate_result_e  invalidate_result;
    ngx_uint_t                                status;

    pd = data;
    ctx = pd->ctx;

    if (pd->handled) {
        return NGX_OK;
    }

    pd->handled = 1;

    /*
     * If the refresh context has been finalized (e.g. request pool being
     * destroyed), chunk pools may already be freed.  Do not access pd->file
     * or any chunk-pool-allocated data.
     */
    if (ctx->finalized) {
        return NGX_OK;
    }

    file = pd->file;

    if (!pd->validation_ready) {
        ctx->errors++;
        if (ctx->active > 0) {
            ctx->active--;
        }

        return NGX_OK;
    }

    /*
     * Determine upstream response status.
     * If upstream returned a response, check status_n.
     * If subrequest failed (timeout, connect error), status_n will be 0.
     */
    status = 0;
    if (r->upstream && r->upstream->headers_in.status_n) {
        status = r->upstream->headers_in.status_n;
    } else if (r->headers_out.status) {
        status = r->headers_out.status;
    }

    if (status == NGX_HTTP_NOT_MODIFIED) {
        /* 304 — cache is still fresh, keep it */
        ctx->refreshed++;
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "refresh: 304 kept \"%V\"", &file->uri);
    } else if (status == NGX_HTTP_OK) {
        /* 200 — content changed, invalidate through unified helper */
        ngx_pool_t *pool;

        pool = ngx_create_pool(4096, r->connection->log);
        if (pool == NULL) {
            ctx->errors++;
        } else if (ngx_http_cache_purge_invalidate_item(ctx->request,
                                                        ctx->cache,
                                                        pool,
                                                        &file->item,
                                                        &invalidate_result)
                   != NGX_OK)
        {
            ctx->errors++;
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0,
                          "refresh invalidate failed for \"%V\"", &file->uri);
        } else if (invalidate_result
                   == NGX_HTTP_CACHE_PURGE_INVALIDATE_PURGED)
        {
            ctx->purged++;
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "refresh: 200 purged \"%V\"", &file->uri);
        } else if (invalidate_result
                   == NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_MISSING
                   || invalidate_result
                   == NGX_HTTP_CACHE_PURGE_INVALIDATE_RACED_REPLACED)
        {
            ctx->refreshed++;
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "refresh: 200 race-kept (%ui) \"%V\"",
                           invalidate_result, &file->uri);
        } else {
            ctx->errors++;
        }

        if (pool != NULL) {
            if (ngx_http_cache_purge_enqueue_temp_pool(&ctx->temp_pools,
                    ctx->request->pool, pool)
                != NGX_OK)
            {
                ngx_destroy_pool(pool);
                ctx->errors++;
            }
        }
    } else {
        /* Error or unexpected status — keep cache (conservative) */
        ctx->errors++;
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "refresh: %ui error kept \"%V\"", status, &file->uri);
    }

    if (ctx->active > 0) {
        ctx->active--;
    }

    if (ctx->timed_out) {
        if (ctx->current < ctx->queued) {
            ctx->errors += ctx->queued - ctx->current;
            ctx->current = ctx->queued;
        }
    }

    /*
     * Background subrequests do not post the parent request upon
     * finalization (unlike WAITED subrequests which go through the
     * postpone filter).  We must manually post the parent request
     * so that its write_event_handler (refresh_start) runs on the
     * next event loop iteration to dispatch more subrequests or
     * finalize the refresh operation.
     */
    ngx_http_post_request(ctx->request, NULL);

    return NGX_OK;
}


/*
 * Fire a single HEAD subrequest for the next file in the queue.
 */
static ngx_int_t
ngx_http_cache_purge_refresh_fire_subrequest(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_http_cache_purge_loc_conf_t       *cplcf;
    ngx_http_cache_purge_refresh_file_t      *file;
    ngx_http_cache_purge_refresh_post_data_t *pd;
    ngx_http_request_t                       *sr;
    ngx_http_post_subrequest_t               *ps;
    ngx_int_t                                 rc;
    ngx_table_elt_t                          *h;
    u_char                                   *time_buf;

    if (ctx->current >= ctx->queued) {
        return NGX_OK;
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    if (cplcf->refresh_timeout != 0 && ngx_current_msec >= ctx->deadline) {
        ngx_http_cache_purge_refresh_mark_timeout(ctx);
        return NGX_ABORT;
    }

    file = (ngx_http_cache_purge_refresh_file_t *)ctx->files->elts
           + ctx->current;

    /* Allocate post-subrequest callback with wrapper data */
    ps = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
    if (ps == NULL) {
        return NGX_ERROR;
    }

    pd = ngx_palloc(r->pool, sizeof(ngx_http_cache_purge_refresh_post_data_t));
    if (pd == NULL) {
        return NGX_ERROR;
    }
    pd->ctx = ctx;
    pd->file = file;
    pd->validation_ready = 0;
    pd->handled = 0;

    ps->handler = ngx_http_cache_purge_refresh_done;
    ps->data = pd;

    /* Create subrequest */
    /*
     * Use BACKGROUND subrequests to avoid r->main->count overflow.
     *
     * Normal (WAITED) subrequests are added to r->postponed and only
     * finalize (decrementing main->count) when they become the active
     * subrequest via the postpone filter.  Since our write_event_handler
     * never outputs data, the postpone filter never runs, so count
     * accumulates unboundedly — overflowing at 64535 for large file sets.
     *
     * Background subrequests skip the postpone list entirely.  Each one
     * finalizes independently via ngx_http_finalize_connection →
     * ngx_http_close_request → main->count--.  The tradeoff is that
     * background finalization does NOT post the parent request, so we
     * must manually call ngx_http_post_request() in refresh_done to
     * re-trigger the dispatch loop.
     */
    rc = ngx_http_subrequest(r, &file->uri,
                             file->args.len > 0 ? &file->args : NULL,
                             &sr, ps,
                             NGX_HTTP_SUBREQUEST_BACKGROUND);
    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "refresh fire subrequest failed rc=%i uri=\"%V\"",
                      rc, &file->uri);
        ctx->errors++;
        return rc;
    }

    ctx->current++;
    ctx->active++;
    ctx->dispatched++;

    /*
     * Set HEAD method and header_only flag for cache validation.
     *
     * Note: nginx's cache_convert_head (on by default) converts HEAD to GET
     * in ngx_http_upstream_cache() before the cache_bypass check, so the
     * upstream actually receives a GET request.  However, header_only = 1
     * ensures nginx finalizes the subrequest after receiving response headers
     * without reading the body (ngx_http_upstream.c:3280 fast-path when
     * header_only && !cacheable && !store).  The real bandwidth savings come
     * from conditional headers (If-None-Match / If-Modified-Since) which
     * elicit 304 responses with no body.  Forcing true HEAD would require
     * hooking into the upstream init phase or setting proxy_cache_convert_head
     * off globally, both unacceptably invasive.
     */
    sr->method = NGX_HTTP_HEAD;
    sr->method_name = ngx_http_head_method_name;
    sr->header_only = 1;

    /*
     * Inject conditional headers for cache validation.
     *
     * Subrequest's headers_in.headers list is shared with parent.
     * We must re-initialize it as an independent list so that pushing
     * conditional headers does not corrupt the parent's header list.
     * We copy essential headers (Host) and add conditional headers.
     */
    if (ngx_list_init(&sr->headers_in.headers, r->pool, 8,
                      sizeof(ngx_table_elt_t))
        != NGX_OK)
    {
        pd->handled = 1;
        ctx->errors++;
        return NGX_OK;
    }

    /* Copy Host header from parent — required by upstream */
    if (r->headers_in.host != NULL) {
        h = ngx_list_push(&sr->headers_in.headers);
        if (h == NULL) {
            pd->handled = 1;
            ctx->errors++;
            return NGX_OK;
        }
        *h = *r->headers_in.host;
        sr->headers_in.host = h;
    }

    /* Clear inherited shortcut pointers that reference parent's headers */
    sr->headers_in.if_none_match = NULL;
    sr->headers_in.if_modified_since = NULL;

    /* If-None-Match (ETag) */
    if (file->etag.len > 0) {
        h = ngx_list_push(&sr->headers_in.headers);
        if (h == NULL) {
            pd->handled = 1;
            ctx->errors++;
            return NGX_OK;
        }
        h->hash = 1;
        ngx_str_set(&h->key, "If-None-Match");
        h->value = file->etag;
        h->lowcase_key = (u_char *) "if-none-match";
        sr->headers_in.if_none_match = h;
    }

    /* If-Modified-Since */
    if (file->last_modified > 0) {
        h = ngx_list_push(&sr->headers_in.headers);
        if (h == NULL) {
            pd->handled = 1;
            ctx->errors++;
            return NGX_OK;
        }
        time_buf = ngx_pnalloc(r->pool,
                               sizeof("Mon, 28 Sep 1970 06:00:00 GMT"));
        if (time_buf == NULL) {
            pd->handled = 1;
            ctx->errors++;
            return NGX_OK;
        }
        h->hash = 1;
        ngx_str_set(&h->key, "If-Modified-Since");
        h->value.data = time_buf;
        ngx_http_time(time_buf, file->last_modified);
        h->value.len = sizeof("Mon, 28 Sep 1970 06:00:00 GMT") - 1;
        h->lowcase_key = (u_char *) "if-modified-since";
        sr->headers_in.if_modified_since = h;
    }

    pd->validation_ready = 1;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "refresh: fired subrequest for \"%V\" (%ui/%ui)",
                   &file->uri, ctx->current, ctx->queued);

    return NGX_OK;
}


/*
 * Build and send the final refresh response to the client.
 */
static ngx_int_t
ngx_http_cache_purge_refresh_send_response(ngx_http_request_t *r)
{
    ngx_http_cache_purge_refresh_ctx_t  *ctx;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_buf_t                           *b;
    ngx_chain_t                          out;
    ngx_int_t                            rc;
    size_t                               len;
    u_char                              *p;

    ctx = ngx_http_get_module_ctx(r, ngx_http_cache_purge_module);
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);

    /* Calculate response body size */
    if (cplcf->resptype == NGX_REPONSE_TYPE_JSON) {
        /* JSON: {"status":"refresh","total":N,"kept":N,"purged":N,"errors":N} */
        len = sizeof("{\"status\":\"refresh\",\"total\":,\"kept\":,\"purged\":,\"errors\":}")
              + 4 * NGX_INT_T_LEN;

        r->headers_out.content_type.len = ngx_http_cache_purge_content_type_json_size - 1;
        r->headers_out.content_type.data = (u_char *) ngx_http_cache_purge_content_type_json;
    } else {
        /* Text: "Refresh: total=N kept=N purged=N errors=N\n" */
        len = sizeof("Refresh: total= kept= purged= errors=\n")
              + 4 * NGX_INT_T_LEN;

        r->headers_out.content_type.len = ngx_http_cache_purge_content_type_text_size - 1;
        r->headers_out.content_type.data = (u_char *) ngx_http_cache_purge_content_type_text;
    }

    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (cplcf->resptype == NGX_REPONSE_TYPE_JSON) {
        p = ngx_sprintf(b->pos,
            "{\"status\":\"refresh\",\"total\":%ui,\"kept\":%ui,"
            "\"purged\":%ui,\"errors\":%ui}",
            ctx->total, ctx->refreshed, ctx->purged, ctx->errors);
    } else {
        p = ngx_sprintf(b->pos,
            "Refresh: total=%ui kept=%ui purged=%ui errors=%ui\n",
            ctx->total, ctx->refreshed, ctx->purged, ctx->errors);
    }

    b->last = p;
    b->last_buf = 1;
    b->last_in_chain = 1;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = p - b->pos;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


static void
ngx_http_cache_purge_refresh_mark_timeout(ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_http_cache_purge_loc_conf_t  *cplcf;
    ngx_uint_t  skipped;

    if (ctx->timed_out) {
        return;
    }

    ctx->timed_out = 1;
    cplcf = ngx_http_get_module_loc_conf(ctx->request,
                                         ngx_http_cache_purge_module);

    if (ctx->dispatched < ctx->total) {
        skipped = ctx->total - ctx->dispatched;
        ctx->errors += skipped;
        ctx->current = ctx->queued;
        ctx->dispatched = ctx->total;

    } else {
        skipped = 0;
    }

    ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                  "cache purge refresh timed out after %M ms, skipped %ui pending entries",
                  cplcf->refresh_timeout,
                  skipped);
}


static void
ngx_http_cache_purge_refresh_pool_cleanup(void *data)
{
    ngx_http_cache_purge_refresh_ctx_t  *ctx;

    ctx = data;

    if (ctx == NULL) {
        return;
    }

    ctx->finalized = 1;

    if (ctx->timeout_ev.timer_set) {
        ngx_del_timer(&ctx->timeout_ev);
    }

    if (ctx->scan_pool != NULL) {
        ngx_destroy_pool(ctx->scan_pool);
        ctx->scan_pool = NULL;
    }

    ngx_http_cache_purge_drain_temp_pools(&ctx->temp_pools);

    ngx_http_cache_purge_refresh_drain_retired_chunk_pools(ctx);

    if (ctx->chunk_pool != NULL) {
        ngx_destroy_pool(ctx->chunk_pool);
        ctx->chunk_pool = NULL;
    }

    if (ctx->retired_chunk_pool != NULL) {
        ngx_destroy_pool(ctx->retired_chunk_pool);
        ctx->retired_chunk_pool = NULL;
    }
}


static ngx_int_t
ngx_http_cache_purge_refresh_enqueue_retired_chunk_pool(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_pool_t *pool)
{
    ngx_http_cache_purge_refresh_temp_pool_t  *entry;

    entry = ngx_palloc(ctx->request->pool,
                       sizeof(ngx_http_cache_purge_refresh_temp_pool_t));
    if (entry == NULL) {
        return NGX_ERROR;
    }

    entry->pool = pool;
    ngx_queue_insert_tail(&ctx->retired_chunk_pools, &entry->queue);

    return NGX_OK;
}


static void
ngx_http_cache_purge_refresh_drain_retired_chunk_pools(
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_queue_t                            *q;
    ngx_http_cache_purge_refresh_temp_pool_t *entry;

    while (!ngx_queue_empty(&ctx->retired_chunk_pools)) {
        q = ngx_queue_head(&ctx->retired_chunk_pools);
        ngx_queue_remove(q);

        entry = ngx_queue_data(q,
                               ngx_http_cache_purge_refresh_temp_pool_t,
                               queue);

        if (entry->pool != NULL) {
            ngx_destroy_pool(entry->pool);
        }
    }
}


static void
ngx_http_cache_purge_refresh_finalize(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_int_t rc;

    if (ctx->finalized) {
        return;
    }

    ctx->finalized = 1;

    if (ctx->timeout_ev.timer_set) {
        ngx_del_timer(&ctx->timeout_ev);
    }

    rc = ngx_http_cache_purge_refresh_send_response(r);
    ngx_http_finalize_request(r, rc);
}


static ngx_int_t
ngx_http_cache_purge_refresh_enqueue_dir(
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_str_t *path)
{
    ngx_http_cache_purge_refresh_dir_t  *dir;
    u_char                              *p;

    dir = ngx_palloc(ctx->request->pool,
                     sizeof(ngx_http_cache_purge_refresh_dir_t));
    if (dir == NULL) {
        return NGX_ERROR;
    }

    p = ngx_pnalloc(ctx->request->pool, path->len + 1);
    if (p == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(p, path->data, path->len);
    p[path->len] = '\0';

    dir->path.len = path->len;
    dir->path.data = p;

    ngx_queue_insert_tail(&ctx->pending_dirs, &dir->queue);

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_refresh_load_dir(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx, ngx_str_t *path)
{
    ngx_dir_t                                dir;
    ngx_http_cache_purge_refresh_scan_entry_t *entry;
    ngx_str_t                                child;
    u_char                                  *name;
    u_char                                  *p;
    size_t                                   len;
    ngx_int_t                                rc;

    if (ctx->scan_pool != NULL) {
        ngx_destroy_pool(ctx->scan_pool);
        ctx->scan_pool = NULL;
    }

    ctx->scan_pool = ngx_create_pool(4096, r->connection->log);
    if (ctx->scan_pool == NULL) {
        return NGX_ERROR;
    }

    ctx->scan_entries = ngx_array_create(ctx->scan_pool, 64,
                                         sizeof(ngx_http_cache_purge_refresh_scan_entry_t));
    if (ctx->scan_entries == NULL) {
        return NGX_ERROR;
    }

    ctx->scan_index = 0;

    if (ngx_open_dir(path, &dir) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                      ngx_open_dir_n " \"%V\" failed", path);
        return NGX_ERROR;
    }

    rc = NGX_OK;

    for ( ;; ) {
        ngx_set_errno(0);

        if (ngx_read_dir(&dir) == NGX_ERROR) {
            if (ngx_errno != NGX_ENOMOREFILES) {
                ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                              ngx_read_dir_n " \"%V\" failed", path);
                rc = NGX_ERROR;
            }

            break;
        }

        len = ngx_de_namelen(&dir);
        name = ngx_de_name(&dir);

        if (len == 1 && name[0] == '.') {
            continue;
        }

        if (len == 2 && name[0] == '.' && name[1] == '.') {
            continue;
        }

        if (path->len == ctx->cache->path->name.len
            && ngx_strncmp(path->data, ctx->cache->path->name.data,
                           path->len) == 0
            && ngx_de_is_dir(&dir))
        {
            if (len == sizeof("proxy_temp") - 1
                && ngx_strncmp(name, (u_char *) "proxy_temp", len) == 0)
            {
                continue;
            }
        }

        child.len = path->len + 1 + len;
        child.data = ngx_pnalloc(ctx->scan_pool, child.len + 1);
        if (child.data == NULL) {
            rc = NGX_ERROR;
            break;
        }

        p = ngx_cpymem(child.data, path->data, path->len);
        *p++ = '/';
        ngx_memcpy(p, name, len);
        p[len] = '\0';

        if (!dir.valid_info && ngx_de_info(child.data, &dir) == NGX_FILE_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                          ngx_de_info_n " \"%V\" failed", &child);
            continue;
        }

        if (!ngx_de_is_file(&dir) && !ngx_de_is_dir(&dir)) {
            continue;
        }

        entry = ngx_array_push(ctx->scan_entries);
        if (entry == NULL) {
            rc = NGX_ERROR;
            break;
        }

        entry->path = child;
        entry->is_dir = ngx_de_is_dir(&dir);
    }

    if (ngx_close_dir(&dir) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                      ngx_close_dir_n " \"%V\" failed", path);
        if (rc == NGX_OK) {
            rc = NGX_ERROR;
        }
    }

    return rc;
}


static ngx_int_t
ngx_http_cache_purge_refresh_scan_next_chunk(ngx_http_request_t *r,
    ngx_http_cache_purge_refresh_ctx_t *ctx)
{
    ngx_http_cache_purge_refresh_dir_t         *dir;
    ngx_http_cache_purge_refresh_scan_entry_t  *entry;
    ngx_int_t                                   rc;

    if (ctx->scan_done) {
        return NGX_OK;
    }

    if (ctx->retired_chunk_pool != NULL) {
        if (ngx_http_cache_purge_refresh_enqueue_retired_chunk_pool(
                ctx, ctx->retired_chunk_pool)
            != NGX_OK)
        {
            ngx_destroy_pool(ctx->retired_chunk_pool);
        }
        ctx->retired_chunk_pool = NULL;
    }

    if (ctx->chunk_pool != NULL) {
        ctx->retired_chunk_pool = ctx->chunk_pool;
        ctx->chunk_pool = NULL;
    }

    ctx->chunk_pool = ngx_create_pool(4096, r->connection->log);
    if (ctx->chunk_pool == NULL) {
        return NGX_ERROR;
    }

    ctx->files = ngx_array_create(ctx->chunk_pool, ctx->chunk_limit,
                                  sizeof(ngx_http_cache_purge_refresh_file_t));
    if (ctx->files == NULL) {
        return NGX_ERROR;
    }

    ctx->current = 0;
    ctx->queued = 0;

    if (ctx->exact) {
        ctx->scan_done = 1;
        return ngx_http_cache_purge_refresh_collect_open_file(r, ctx);
    }

    if (!ctx->scan_initialized) {
        ngx_queue_init(&ctx->pending_dirs);
        if (ngx_http_cache_purge_refresh_enqueue_dir(ctx,
                                                     &ctx->cache->path->name)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ctx->scan_initialized = 1;
    }

    for ( ;; ) {
        if (ctx->timeout_enabled && !ctx->timed_out
            && ngx_current_msec >= ctx->deadline)
        {
            ngx_http_cache_purge_refresh_mark_timeout(ctx);
            ctx->scan_done = 1;
            return NGX_OK;
        }

        if (ctx->queued >= ctx->chunk_limit) {
            return NGX_OK;
        }

        if (ctx->scan_entries != NULL
            && ctx->scan_index < ctx->scan_entries->nelts)
        {
            entry = ((ngx_http_cache_purge_refresh_scan_entry_t *)
                     ctx->scan_entries->elts) + ctx->scan_index++;

            if (entry->is_dir) {
                if (ngx_http_cache_purge_refresh_enqueue_dir(ctx, &entry->path)
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }

                continue;
            }

            rc = ngx_http_cache_purge_refresh_collect_path(ctx, &entry->path, 0);
            if (rc != NGX_OK) {
                if (rc == NGX_ABORT && ctx->timed_out) {
                    ctx->scan_done = 1;
                    return NGX_OK;
                }

                return rc;
            }

            continue;
        }

        if (ctx->scan_pool != NULL) {
            ngx_destroy_pool(ctx->scan_pool);
            ctx->scan_pool = NULL;
        }

        ctx->scan_entries = NULL;
        ctx->scan_index = 0;

        if (ngx_queue_empty(&ctx->pending_dirs)) {
            ctx->scan_done = 1;
            return NGX_OK;
        }

        dir = (ngx_http_cache_purge_refresh_dir_t *) ngx_queue_data(
                  ngx_queue_head(&ctx->pending_dirs),
                  ngx_http_cache_purge_refresh_dir_t, queue);
        ngx_queue_remove(&dir->queue);

        rc = ngx_http_cache_purge_refresh_load_dir(r, ctx, &dir->path);
        if (rc != NGX_OK) {
            return rc;
        }
    }
}


static void
ngx_http_cache_purge_refresh_timeout_handler(ngx_event_t *ev)
{
    ngx_http_cache_purge_refresh_ctx_t  *ctx;

    ctx = ev->data;

    if (ctx == NULL || ctx->finalized) {
        return;
    }

    ngx_http_cache_purge_refresh_mark_timeout(ctx);

    if (ctx->active == 0) {
        ngx_http_cache_purge_refresh_finalize(ctx->request, ctx);
    }
}


/*
 * Start firing subrequests for the collected file set.
 */
static void
ngx_http_cache_purge_refresh_start(ngx_http_request_t *r)
{
    ngx_http_cache_purge_refresh_ctx_t  *ctx;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_int_t                            rc;
    ngx_uint_t                           i, concurrency;

    ctx = ngx_http_get_module_ctx(r, ngx_http_cache_purge_module);

    if (ctx == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    if (ctx->queued == 0 && ctx->scan_done && ctx->total == 0) {
        ngx_http_cache_purge_refresh_finalize(r, ctx);
        return;
    }

    if (ctx->current >= ctx->queued && ctx->active == 0 && ctx->scan_done) {
        ngx_http_cache_purge_refresh_finalize(r, ctx);
        return;
    }

    if (ctx->timed_out) {
        if (ctx->active == 0) {
            ngx_http_cache_purge_refresh_finalize(r, ctx);
        }
        return;
    }

    if (ctx->active == 0 && ctx->current >= ctx->queued && !ctx->scan_done) {
        ctx->current = 0;
        ctx->queued = 0;
    }

    if (ctx->queued == 0) {
        if (!ctx->scan_done) {
            rc = ngx_http_cache_purge_refresh_scan_next_chunk(r, ctx);
            if (rc != NGX_OK) {
                ctx->errors++;
                ngx_http_cache_purge_refresh_finalize(r, ctx);
                return;
            }
        }

        if (ctx->timed_out) {
            if (ctx->queued > 0) {
                ctx->errors += ctx->queued;
                ctx->current = ctx->queued;
            }

            if (ctx->active == 0) {
                ngx_http_cache_purge_refresh_finalize(r, ctx);
            }
            return;
        }

        if (ctx->queued == 0) {
            if (ctx->scan_done) {
                ngx_http_cache_purge_refresh_finalize(r, ctx);
            }
            return;
        }
    }

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    concurrency = cplcf->refresh_concurrency;
    if (concurrency == 0) {
        concurrency = 1;
    }
    if (concurrency > ctx->queued) {
        concurrency = ctx->queued;
    }

    for (i = ctx->active; i < concurrency; i++) {

        rc = ngx_http_cache_purge_refresh_fire_subrequest(r, ctx);
        if (rc == NGX_OK) {
            continue;
        }

        if (rc == NGX_ABORT && ctx->timed_out) {
            break;
        }

        ctx->errors += ctx->queued - ctx->current;
        ngx_http_cache_purge_refresh_finalize(r, ctx);
        return;
    }
}


/*
 * Main entry point for the refresh feature.
 * Called from backend purge handlers when refresh mode is active.
 * Supports exact refresh, partial refresh, and refresh_all.
 */
static ngx_int_t
ngx_http_cache_purge_refresh(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache)
{
    ngx_http_cache_purge_refresh_ctx_t  *ctx;
    ngx_http_cache_purge_loc_conf_t     *cplcf;
    ngx_pool_cleanup_t                  *cln;
    ngx_str_t                           *keys;
    ngx_str_t                            key;
    ngx_str_t                            tail;

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "cache purge refresh in %s", cache->path->name.data);

    /* Allocate refresh context */
    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_cache_purge_refresh_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_queue_init(&ctx->temp_pools);
    ngx_queue_init(&ctx->retired_chunk_pools);

    ctx->request = r;
    ctx->cache = cache;

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    cln->handler = ngx_http_cache_purge_refresh_pool_cleanup;
    cln->data = ctx;

    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    ctx->purge_all = cplcf->conf->purge_all;
    ctx->exact = !ctx->purge_all && !ngx_http_cache_purge_is_partial(r);
    ctx->timeout_enabled = (cplcf->refresh_timeout != 0);
    ctx->deadline = ngx_current_msec + cplcf->refresh_timeout;
    ctx->chunk_limit = cplcf->refresh_concurrency * 4;
    if (ctx->chunk_limit == 0) {
        ctx->chunk_limit = 4;
    }
    if (ctx->chunk_limit < 32) {
        ctx->chunk_limit = 32;
    }

    ngx_memzero(&ctx->timeout_ev, sizeof(ngx_event_t));
    ctx->timeout_ev.handler = ngx_http_cache_purge_refresh_timeout_handler;
    ctx->timeout_ev.data = ctx;
    ctx->timeout_ev.log = r->connection->log;

    if (ctx->timeout_enabled) {
        ngx_add_timer(&ctx->timeout_ev, cplcf->refresh_timeout);
    }

    /* Get the evaluated cache key. Strip trailing '*' only for partial refresh. */
    keys = r->cache->keys.elts;
    key = keys[0];
    if (!ctx->exact && key.len > 0 && key.data[key.len - 1] == '*') {
        key.len--;
    }

    /* Store exact key or partial prefix for later matching. */
    ctx->key_partial.len = key.len;
    ctx->key_partial.data = key.data;

    /*
     * Infer the non-URI prefix in cache key.
     * We first try unparsed_uri (keeps args), then uri.
     * If neither is at key tail, fall back to 0.
     */
    tail = r->unparsed_uri;
    if (!ctx->exact && tail.len > 0 && tail.data[tail.len - 1] == '*') {
        tail.len--;
    }

    if (tail.len > 0 && key.len >= tail.len
        && ngx_strncmp(key.data + key.len - tail.len, tail.data,
                       tail.len) == 0)
    {
        ctx->key_prefix_len = key.len - tail.len;

    } else {
        tail = r->uri;
        if (!ctx->exact && tail.len > 0 && tail.data[tail.len - 1] == '*') {
            tail.len--;
        }

        if (tail.len > 0 && key.len >= tail.len
            && ngx_strncmp(key.data + key.len - tail.len, tail.data,
                           tail.len) == 0)
        {
            ctx->key_prefix_len = key.len - tail.len;

        } else {
            ctx->key_prefix_len = 0;
        }
    }

    /* Set module context on the request */
    ngx_http_set_ctx(r, ctx, ngx_http_cache_purge_module);

    /* Set up the write event handler and start bounded subrequests */
    r->write_event_handler = ngx_http_cache_purge_refresh_start;

#if (nginx_version >= 8011)
    r->main->count++;
#endif

    ngx_http_cache_purge_refresh_start(r);

    return NGX_DONE;
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
