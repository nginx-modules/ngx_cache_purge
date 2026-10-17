#ifndef _NGX_CACHE_PURGE_H_INCLUDED_
#define _NGX_CACHE_PURGE_H_INCLUDED_

/* Private contracts shared by the module's separately compiled components. */
#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define NGX_CACHE_PURGE_RESPONSE_TYPE_HTML  1
#define NGX_CACHE_PURGE_RESPONSE_TYPE_XML   2
#define NGX_CACHE_PURGE_RESPONSE_TYPE_JSON  3
#define NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT  4
#define NGX_CACHE_PURGE_RESPONSE_TYPE_NONE  5

#define NGX_CACHE_PURGE_QUEUE_SIZE_DEFAULT   1024
#define NGX_CACHE_PURGE_BATCH_SIZE_DEFAULT   10
#define NGX_CACHE_PURGE_BATCH_SIZE_MAX       64
/*
 * A bare integer denotes milliseconds, as does an explicit ms ending.
 */
#define NGX_CACHE_PURGE_THROTTLE_MS_DEFAULT  10  /* milliseconds */
#define NGX_CACHE_PURGE_KEY_MAX_LEN          512
#define NGX_CACHE_PURGE_ENTRY_KEY_MAX        8192
#define NGX_CACHE_PURGE_STATUS_BODY_MAX      8192
/* Widest "namespaces" entry (32-byte name) and the closing text after the
 * last entry; the list stops early and says so instead of failing the reply. */
#define NGX_CACHE_PURGE_STATUS_NS_MAX        160
#define NGX_CACHE_PURGE_STATUS_NS_TAIL       48
#define NGX_CACHE_PURGE_SHM_VERSION          7
#define NGX_CACHE_PURGE_REFRESH_BATCH        32
#define NGX_CACHE_PURGE_REFRESH_PROBES       16
#define NGX_CACHE_PURGE_QUEUE_TIMEOUT        60000   /* ms */
#define NGX_CACHE_PURGE_WALK_ENTRIES         64
#define NGX_CACHE_PURGE_WALK_MSEC            5
/*
 * Cache file format:
 *
 *     binary header | "\nKEY: " | key | "\n" | cached response
 *
 * The offset skips the six-byte marker after the binary header.
 */
#define NGX_CACHE_PURGE_KEY_HDR_OFFSET       6

/*
 * Reserve at least eight native pages for slab metadata and task storage.
 * The exact slab overhead depends on the host and nginx version.
 */
#define NGX_CACHE_PURGE_SHM_MIN_PAGES        8


#if (NGX_HTTP_CACHE)
#include "ngx_cache_purge_invalidation.h"
#if (NGX_WIN32)
#include "ngx_cache_purge_win32.h"
#else
#include "ngx_cache_purge_posix.h"
#endif

extern ngx_module_t ngx_http_cache_purge_module;

/* -- forward declarations ----------------------------------------------- */

typedef struct ngx_http_cache_purge_queue_item_s
    ngx_http_cache_purge_queue_item_t;
typedef struct ngx_http_cache_purge_queue_s      ngx_http_cache_purge_queue_t;
typedef struct ngx_http_cache_purge_main_conf_s
    ngx_http_cache_purge_main_conf_t;
typedef struct ngx_http_cache_purge_path_s       ngx_http_cache_purge_path_t;
typedef struct ngx_http_cache_purge_protection_s
    ngx_http_cache_purge_protection_t;


/* One wire contract for every module-owned administrative response. */
typedef enum {
    NGX_PURGE_RESULT_PURGED,
    NGX_PURGE_RESULT_NO_MATCH,
    NGX_PURGE_RESULT_ACCEPTED,
    NGX_PURGE_RESULT_INVALIDATED,
    NGX_PURGE_RESULT_STATUS,
    NGX_PURGE_RESULT_REFRESH_COMPLETE,
    NGX_PURGE_RESULT_REFRESH_INCOMPLETE,
    NGX_PURGE_RESULT_INVALID_SELECTOR,
    NGX_PURGE_RESULT_INVALID_CURSOR,
    NGX_PURGE_RESULT_METHOD_NOT_ALLOWED,
    NGX_PURGE_RESULT_ACCESS_DENIED,
    NGX_PURGE_RESULT_CACHE_UNAVAILABLE,
    NGX_PURGE_RESULT_KEY_MISMATCH,
    NGX_PURGE_RESULT_DELETE_FAILED,
    NGX_PURGE_RESULT_SCAN_FAILED,
    NGX_PURGE_RESULT_INTERNAL_ERROR,
    NGX_PURGE_RESULT_COUNT
} ngx_http_cache_purge_result_code_t;

typedef struct {
    ngx_http_cache_purge_result_code_t  code;
    ngx_uint_t                        deleted;
    ngx_uint_t                        delete_errors;
    ngx_uint_t                        scan_errors;
    unsigned                          counted:1;
} ngx_http_cache_purge_result_t;

/* -- data structures ---------------------------------------------------- */

struct ngx_http_cache_purge_queue_item_s {
    ngx_str_node_t                      node;
    ngx_queue_t                         link;
    ngx_str_t                           pool_name;
    ngx_flag_t                          purge_all;
    ngx_flag_t                          running;
    ngx_flag_t                          rerun;
    ngx_pid_t                           owner;
    ngx_uint_t                          owner_slot;
    uint64_t                            owner_epoch;
    ngx_msec_t                          enqueued_at;
    ngx_msec_t                          retry_at;
    ngx_uint_t                          retries;
    ngx_http_cache_purge_path_t        *path;
    ngx_http_cache_purge_protection_t  *protection;
    ngx_http_cache_purge_protection_t  *next_protection;
};

enum {
    NGX_PURGE_ENQUEUED, NGX_PURGE_ENQUEUED_FULL,
    NGX_PURGE_STARTED, NGX_PURGE_STARTED_FULL, NGX_PURGE_FINISHED,
    NGX_PURGE_DEDUPLICATED, NGX_PURGE_WAITING, NGX_PURGE_OVERFLOW,
    NGX_PURGE_DELETED, NGX_PURGE_DELETE_ERRORS, NGX_PURGE_DELETED_BYTES,
    NGX_PURGE_HIT, NGX_PURGE_MISS, NGX_PURGE_INVALIDATED,
    NGX_PURGE_SCAN_ERRORS, NGX_PURGE_RETRIED, NGX_PURGE_EXHAUSTED,
    NGX_PURGE_METRICS
};

/* One event-loop writer per slot; pool threads only update their work context. */
typedef struct {
    volatile ngx_uint_t  sequence;
    volatile uint64_t    value[NGX_PURGE_METRICS];
} ngx_http_cache_purge_stats_t;

typedef struct {
    uint64_t  pending, pending_full, active;
    uint64_t  overflow, deleted, deduplicated, waiting;
    uint64_t  errors, bytes, hits, misses;
    uint64_t  invalidated;
    uint64_t  scan_errors, retries, exhausted;
} ngx_http_cache_purge_cache_stat_t;

/* The zone owns names independently of pending tasks. */
struct ngx_http_cache_purge_path_s {
    ngx_str_t                         name;
    u_char                           *stats;
    size_t                            stats_stride;
    /* Pending and running tasks remain indexed until final completion. */
    ngx_rbtree_t                      rbtree;
    ngx_rbtree_node_t                 sentinel;
    ngx_http_cache_purge_path_t       *next;
};

/* A task retains the path settings captured when it was accepted. */
struct ngx_http_cache_purge_protection_s {
    ngx_str_t                          *names;
    ngx_uint_t                          nelts;
    ngx_http_cache_purge_protection_t  *next;
};

struct ngx_http_cache_purge_queue_s {
    ngx_uint_t                          version;
    ngx_queue_t                         pending;
    ngx_queue_t                         active;
    /*
     * The zone lock protects all pending and active task state.
     * Namespace publication only uses a nested trylock, never waits. No
     * reverse namespace-to-queue acquisition is allowed.
     */
    ngx_uint_t                          size;
    ngx_uint_t                          running;
    ngx_msec_t                          oldest;
    ngx_pid_t                           owners[NGX_MAX_PROCESSES];
    uint64_t                            epochs[NGX_MAX_PROCESSES];
    ngx_uint_t                          probes[NGX_MAX_PROCESSES];
    ngx_slab_pool_t                    *shpool;
    ngx_uint_t                          max_size;
    ngx_http_cache_purge_path_t        *paths;
    ngx_http_cache_purge_protection_t  *protections;
};

struct ngx_http_cache_purge_main_conf_s {
    ngx_http_cache_purge_queue_t  *queue;
    ngx_shm_zone_t                *shm_zone;
    ngx_uint_t                     queue_size;
    ngx_uint_t                     batch_size;
    ngx_msec_t                     throttle_ms;
    ngx_flag_t                     background_purge;
    ngx_flag_t                     legacy_status_seen;
    /*
     * vary_aware: when on, an exact-key purge walks the cache directory after
     * trying the primary entry and removes any remaining files that carry the
     * same KEY: string (i.e. Vary / gzip_vary variants at different paths).
     * Disabled by default because it adds a full cache walk per purge request.
     */
    ngx_flag_t                          vary_aware;
    ngx_array_t                         queue_status_locations;
    ngx_array_t                         namespaces;
    ngx_array_t                        *paths;
    ngx_http_cache_purge_protection_t  *protection;
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

    ngx_http_cache_purge_conf_t  *conf;
    ngx_http_handler_pt           handler;
    ngx_http_handler_pt           original_handler;
    ngx_uint_t                    response_type;
    ngx_str_t                     queue_status_cache_path;
    ngx_http_cache_purge_path_t  *queue_status_path;
    ngx_str_t                    thread_pool_name;
    ngx_http_cache_purge_namespace_t *namespace;
    ngx_http_complex_value_t          logical_key;
# if (NGX_HTTP_PROXY)
    ngx_flag_t                   refresh;
    ngx_str_t                    refresh_location;
    ngx_str_t                    refresh_prefix;
    ngx_msec_t                   refresh_timeout;
# endif

# if (NGX_HTTP_PROXY)
    /*
     * Keep separate-location cache state independent of proxy_pass state.
     * This avoids altering core proxy location inheritance.
     */
    ngx_shm_zone_t            *proxy_separate_zone;   /* static zone name  */
    ngx_http_complex_value_t  *proxy_separate_value;  /* dynamic zone expr */
    ngx_http_complex_value_t   proxy_separate_key;    /* purge key template*/
# endif
} ngx_http_cache_purge_loc_conf_t;

typedef struct {
    u_char                             *key_partial;
    ngx_uint_t                          key_len;
    u_char                              key_buffer[NGX_CACHE_PURGE_ENTRY_KEY_MAX + 1];
    size_t                              entry_key_len;
    ngx_http_file_cache_header_t        header;
    ngx_uint_t                          files_deleted;
    ngx_uint_t                          delete_errors;
    ngx_uint_t                          scan_errors;
    uint64_t                            bytes_deleted;
    off_t                               file_size;
    ngx_str_t                           cache_root;
    ngx_http_cache_purge_protection_t  *protection;
    ngx_array_t                        *paths;
    ngx_log_t                          *log;
    ngx_dir_t                          *parent_dir;
    ngx_http_cache_purge_namespace_t   *namespace;
    void                            (*visit)(void *ctx, ngx_str_t *path);
    /* Exact Vary matching also invalidates native cache nodes. */
    ngx_http_file_cache_t  *cache;
} ngx_http_cache_purge_walk_ctx_t;

typedef struct ngx_http_cache_purge_dir_s  ngx_http_cache_purge_dir_t;

struct ngx_http_cache_purge_dir_s {
    ngx_dir_t                    dir;
    size_t                       len;
    ngx_http_cache_purge_dir_t  *parent;
};

typedef struct {
    ngx_http_cache_purge_walk_ctx_t  *ctx;
    ngx_http_cache_purge_dir_t       *top;
    ngx_str_t                        path;
    size_t                           allocated;
    unsigned                         started:1;
    unsigned                         complete:1;
} ngx_http_cache_purge_cursor_t;

typedef struct {
    ngx_http_request_t             *request;
    ngx_event_t                     event;
    ngx_http_cleanup_t             *cleanup;
    ngx_http_cache_purge_path_t     *path;
} ngx_http_cache_purge_request_t;

typedef struct {
    ngx_event_t                         event;
    ngx_http_cache_purge_main_conf_t    *conf;
    ngx_http_cache_purge_queue_item_t   *item;
    ngx_http_cache_purge_walk_ctx_t      ctx;
    ngx_http_cache_purge_cursor_t        cursor;
    ngx_uint_t                          batch;
    ngx_msec_t                          recover_at;
# if (NGX_THREADS && !NGX_WIN32)
    ngx_thread_pool_t                  *pool;
    ngx_thread_task_t                  *task;
    ngx_atomic_t                        finished;
# endif
} ngx_http_cache_purge_work_t;

/* Snapshot of pending and active task state. */
typedef struct {
    struct {
        ngx_uint_t  size;
        ngx_uint_t  max_size;
        ngx_uint_t  running;
        ngx_msec_t  wait_time;
        ngx_flag_t  full;
    } queue;

    ngx_http_cache_purge_cache_stat_t  cache;
} ngx_http_cache_purge_queue_stat_t;


static ngx_inline ngx_uint_t
ngx_http_cache_purge_path_equal(u_char *a, u_char *b, size_t len)
{
    size_t  i;

    for (i = 0; i < len; i++) {
        if (ngx_http_cache_purge_path_char(a[i])
            != ngx_http_cache_purge_path_char(b[i]))
        {
            return 0;
        }
    }

    return 1;
}


static ngx_inline size_t
ngx_http_cache_purge_path_length(const ngx_str_t *path)
{
    size_t  len, min;

    len = path->len;
    min = ngx_http_cache_purge_path_min_length(path);

    while (len > min
           && ngx_http_cache_purge_path_separator(path->data[len - 1]))
    {
        len--;
    }

    return len;
}


static ngx_inline ngx_uint_t
ngx_http_cache_purge_path_contains(const ngx_str_t *path,
    const ngx_str_t *root)
{
    size_t  len;

    len = ngx_http_cache_purge_path_length(root);
    if (len == 0 || path->len < len
        || !ngx_http_cache_purge_path_equal(path->data, root->data, len))
    {
        return 0;
    }

    return path->len == len
           || ngx_http_cache_purge_path_separator(root->data[len - 1])
           || ngx_http_cache_purge_path_separator(path->data[len]);
}

ngx_int_t
ngx_http_cache_purge_access_handler(ngx_http_request_t *r);
ngx_int_t
ngx_http_cache_purge_delete_variants(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_http_cache_purge_walk_ctx_t *ctx);
#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
ngx_int_t
ngx_http_cache_purge_enqueue(ngx_http_request_t *r,
    ngx_http_file_cache_t *cache, ngx_str_t *key, ngx_flag_t purge_all, ngx_str_t *expected);
#endif
void
ngx_http_cache_purge_exit_worker(ngx_cycle_t *cycle);
ngx_int_t
ngx_http_cache_purge_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data);
ngx_int_t
ngx_http_cache_purge_init_worker(ngx_cycle_t *cycle);
ngx_uint_t
ngx_http_cache_purge_is_cache_file(ngx_str_t *path);
ngx_uint_t
ngx_http_cache_purge_is_protected_path(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path);
ngx_int_t
ngx_http_cache_purge_log(ngx_http_request_t *r);
ngx_int_t
ngx_http_cache_purge_probe_credit(ngx_http_cache_purge_queue_t *queue,
    ngx_flag_t take);
ngx_int_t
ngx_http_cache_purge_queue_status_handler(ngx_http_request_t *r);
ssize_t
ngx_http_cache_purge_read_key(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *path, size_t len);
ngx_int_t
ngx_http_cache_purge_send_result(ngx_http_request_t *r,
    ngx_http_cache_purge_result_t *result);
ngx_int_t
ngx_http_cache_purge_reply(ngx_http_request_t *r,
    ngx_http_cache_purge_result_code_t code);
ngx_int_t
ngx_http_cache_purge_response_headers(ngx_http_request_t *r,
    ngx_http_cache_purge_result_code_t code);
ngx_int_t
ngx_http_cache_purge_method_response(ngx_http_request_t *r, ngx_str_t *allow);
void
ngx_http_cache_purge_stat_add(ngx_http_cache_purge_path_t *path,
    ngx_uint_t field, uint64_t value);
#if (NGX_HTTP_FASTCGI || NGX_HTTP_PROXY || NGX_HTTP_SCGI || NGX_HTTP_UWSGI)
void
ngx_http_cache_purge_stat_wait(ngx_http_cache_purge_path_t *path, ngx_int_t delta);
#endif
ngx_flag_t
ngx_http_cache_purge_threaded(void);
ngx_int_t
ngx_http_cache_purge_unlink(ngx_log_t *log, u_char *name, ngx_dir_t *parent);
ngx_int_t
ngx_http_cache_purge_walk(ngx_http_cache_purge_walk_ctx_t *ctx);
void
ngx_http_cache_purge_walk_close(ngx_http_cache_purge_cursor_t *cursor);
void
ngx_http_cache_purge_walk_init(ngx_http_cache_purge_walk_ctx_t *ctx,
    ngx_str_t *root, ngx_http_cache_purge_protection_t *protection,
    ngx_array_t *paths, ngx_log_t *log);
ngx_int_t
ngx_http_cache_purge_walk_step(ngx_http_cache_purge_cursor_t *cursor,
    ngx_uint_t limit);
#if (NGX_HTTP_PROXY)
char *
ngx_http_cache_refresh_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
#endif
#if (NGX_HTTP_PROXY)
ngx_int_t
ngx_http_cache_refresh_guard(ngx_http_request_t *r);
#endif
#if (NGX_HTTP_PROXY)
ngx_int_t
ngx_http_cache_refresh_handler(ngx_http_request_t *r);
#endif
#if (NGX_HTTP_PROXY)
char *
ngx_http_cache_refresh_profile(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
#endif
#if (NGX_HTTP_PROXY)
ngx_int_t
ngx_http_cache_refresh_start(ngx_http_request_t *r);
#endif
#if (NGX_HTTP_PROXY)
ngx_int_t
ngx_http_cache_refresh_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
#endif
#if (NGX_HTTP_FASTCGI)
ngx_int_t
ngx_http_fastcgi_cache_purge_handler(ngx_http_request_t *r);
#endif
#if (NGX_HTTP_PROXY)
char *
ngx_http_proxy_cache_purge_conf(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
#endif
#if (NGX_HTTP_PROXY)
ngx_int_t
ngx_http_proxy_cache_purge_handler(ngx_http_request_t *r);
#endif
#if (NGX_HTTP_SCGI)
ngx_int_t
ngx_http_scgi_cache_purge_handler(ngx_http_request_t *r);
#endif
#if (NGX_HTTP_UWSGI)
ngx_int_t
ngx_http_uwsgi_cache_purge_handler(ngx_http_request_t *r);
#endif

#endif /* NGX_HTTP_CACHE */
#endif
