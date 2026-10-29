/*
 * Copyright (C) 2026, Denys Denysov
 * Distributed under the same BSD license as ngx_cache_purge_module.c.
 */
#include <ngx_config.h>
#include "ngx_cache_purge_invalidation.h"

#if (NGX_HTTP_CACHE)

extern ngx_module_t ngx_http_cache_purge_module;

typedef struct ngx_http_cache_purge_rule_s ngx_http_cache_purge_rule_t;
struct ngx_http_cache_purge_rule_s {
    ngx_http_cache_purge_rule_t  *child, *sibling, *allocated;
    uint64_t                     generation;
    u_char                       byte;
};

struct ngx_http_cache_purge_generation_s {
    ngx_uint_t                   version;
    uint64_t                     serial, baseline;
    u_char                       epoch[10];
    ngx_str_t                    state_path;
    ngx_http_cache_purge_rule_t  *exact, *prefix, *allocated, *tail, *garbage;
};

/*
 * Namespace state that outlives a worker: the epoch, the generation
 * counters and the rule trie.  Losing them on a full restart resets every
 * key to the baseline generation, which makes all existing cache files
 * unreachable.  Persisting them keeps the reachable set identical while
 * still leaving invalidated files unreachable, because their generation
 * stays below the restored value.
 *
 * The snapshot is written on graceful shutdown only.  A run that does not
 * reach the shutdown hook leaves the "running" marker behind, and the next
 * start rotates the epoch instead of trusting a stale snapshot.
 */
typedef struct ngx_http_cache_purge_snapshot_s ngx_http_cache_purge_snapshot_t;
struct ngx_http_cache_purge_snapshot_s {
    ngx_http_cache_purge_snapshot_t   *next;
    ngx_http_cache_purge_namespace_t  *ns;
    ngx_str_t                         logical, encoded;
};

typedef struct {
    ngx_http_request_t               *request;
    ngx_http_cache_purge_snapshot_t  *snapshots;
    ngx_flag_t                        evaluating;
} ngx_http_cache_purge_generation_request_t;


/* -- persisted namespace state ----------------------------------------- */

/*
 * Namespace state that outlives a worker: the epoch, the generation
 * counters and the rule trie.  Losing them on a full restart resets every
 * key to the baseline generation, which makes all existing cache files
 * unreachable.  Persisting them keeps the reachable set identical while
 * still leaving invalidated files unreachable, because their generation
 * stays below the restored value.
 *
 * The snapshot is written on graceful shutdown only.  A run that does not
 * reach the shutdown hook leaves the running marker behind, and the next
 * start rotates the epoch instead of trusting a stale snapshot.
 *
 * Every artifact is namespace-specific: a state directory may be shared by
 * several namespaces, and they must not overwrite each other.
 */
#define NGX_CACHE_PURGE_STATE_SUFFIX   "state"
#define NGX_CACHE_PURGE_TMP_SUFFIX     "tmp"
#define NGX_CACHE_PURGE_RUNNING_SUFFIX "running"
#define NGX_CACHE_PURGE_STATE_MAGIC    0x4e435047u  /* "NCPG" */
#define NGX_CACHE_PURGE_STATE_VERSION  2
#define NGX_CACHE_PURGE_NAMESPACE_VERSION  2
#define NGX_CACHE_PURGE_STATE_MAX      (64 * 1024 * 1024)
#define NGX_CACHE_PURGE_STATE_HEADER   64
#define NGX_CACHE_PURGE_STATE_CRC      56

typedef ngx_int_t (*ngx_http_cache_purge_rule_visit_pt)(void *ctx,
    const u_char *key, size_t len, uint64_t generation);

typedef struct {
    ngx_http_cache_purge_rule_t  *node;
    size_t                        depth;
} ngx_http_cache_purge_rule_frame_t;

typedef struct {
    u_char      *p;
    size_t       size;
    ngx_uint_t   count;
    u_char       kind;
} ngx_http_cache_purge_rule_write_t;


static u_char *
ngx_http_cache_purge_put32(u_char *p, uint32_t v)
{
    *p++ = (u_char) (v >> 24);
    *p++ = (u_char) (v >> 16);
    *p++ = (u_char) (v >> 8);
    *p++ = (u_char) v;
    return p;
}


static u_char *
ngx_http_cache_purge_put64(u_char *p, uint64_t v)
{
    p = ngx_http_cache_purge_put32(p, (uint32_t) (v >> 32));
    return ngx_http_cache_purge_put32(p, (uint32_t) v);
}


static uint32_t
ngx_http_cache_purge_get32(const u_char *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
           | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}


static uint64_t
ngx_http_cache_purge_get64(const u_char *p)
{
    return ((uint64_t) ngx_http_cache_purge_get32(p) << 32)
           | ngx_http_cache_purge_get32(p + 4);
}


/*
 * Iterative pre-order walk over one rule trie.  An explicit frame array
 * keeps the stack bounded by the longest stored selector, which can reach
 * the logical key limit, so recursion is not safe here.
 */
static ngx_int_t
ngx_http_cache_purge_rule_walk(ngx_http_cache_purge_generation_t *state,
    ngx_http_cache_purge_rule_t *root, u_char *path, size_t capacity,
    ngx_http_cache_purge_rule_visit_pt visit, void *ctx, ngx_log_t *log)
{
    ngx_http_cache_purge_rule_frame_t  *frames, *frame;
    ngx_http_cache_purge_rule_t        *node;
    size_t                              depth, sp;
    ngx_int_t                           rc;

    if (root == NULL) {
        return NGX_OK;
    }

    frames = ngx_alloc((capacity + 2) * sizeof(*frames), log);
    if (frames == NULL) {
        return NGX_ERROR;
    }

    frames[0].node = root;
    frames[0].depth = 0;
    sp = 1;

    for ( ;; ) {
        if (sp == 0) {
            break;
        }

        frame = &frames[sp - 1];
        node = frame->node;

        if (node == NULL) {
            sp--;
            continue;
        }

        frame->node = node->sibling;
        depth = frame->depth;

        if (depth >= capacity) {
            ngx_free(frames);
            return NGX_ERROR;
        }

        path[depth] = node->byte;

        if (node->generation > state->baseline) {
            rc = visit(ctx, path, depth + 1, node->generation);
            if (rc != NGX_OK) {
                ngx_free(frames);
                return rc;
            }
        }

        if (node->child != NULL) {
            if (sp + 1 >= capacity + 2) {
                ngx_free(frames);
                return NGX_ERROR;
            }
            frames[sp].node = node->child;
            frames[sp].depth = depth + 1;
            sp++;
        }
    }

    ngx_free(frames);
    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_rule_write(void *data, const u_char *key, size_t len,
    uint64_t generation)
{
    ngx_http_cache_purge_rule_write_t *ctx = data;

    ctx->size += 1 + 2 + 8 + len;
    ctx->count++;

    if (ctx->p == NULL) {
        return NGX_OK;
    }

    *ctx->p++ = ctx->kind;
    *ctx->p++ = (u_char) (len >> 8);
    *ctx->p++ = (u_char) len;
    ctx->p = ngx_http_cache_purge_put64(ctx->p, generation);
    ngx_memcpy(ctx->p, key, len);
    ctx->p += len;

    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_state_path(ngx_http_cache_purge_namespace_t *ns,
    const char *suffix, u_char *buf, size_t size)
{
    u_char  *p;

    p = ngx_snprintf(buf, size, "%V/%V.%s", &ns->state_path->name,
                     &ns->name, suffix);
    if (p == buf + size) {
        return NGX_ERROR;
    }

    *p = '\0';
    return NGX_OK;
}


/*
 * A rename or a marker creation is only durable once the directory entry
 * itself is on disk.  Without this sync a power loss can leave an older
 * snapshot with no running marker, which would restore stale rules.
 */
static ngx_int_t
ngx_http_cache_purge_sync_state_dir(ngx_http_cache_purge_namespace_t *ns)
{
#if (NGX_WIN32)
    (void) ns;
    return NGX_OK;
#else
    u_char    path[NGX_MAX_PATH + 1];
    ngx_fd_t  fd;
    ngx_int_t rc;

    if (ns->state_path->name.len + 1 > sizeof(path)) {
        return NGX_ERROR;
    }

    ngx_memcpy(path, ns->state_path->name.data, ns->state_path->name.len);
    path[ns->state_path->name.len] = '\0';

    fd = ngx_open_file(path, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, ngx_errno,
                      "ngx_cache_purge: cannot open state directory \"%s\"",
                      path);
        return NGX_ERROR;
    }

    rc = fsync(fd) == -1 ? NGX_ERROR : NGX_OK;

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        rc = NGX_ERROR;
    }

    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, ngx_errno,
                      "ngx_cache_purge: cannot sync state directory \"%s\"",
                      path);
    }

    return rc;
#endif
}


void
ngx_http_cache_purge_generation_clear_running(ngx_http_cache_purge_namespace_t *ns,
    ngx_pid_t owner)
{
    u_char    path[NGX_MAX_PATH + 1];
    u_char    buf[32];
    ssize_t   n;
    ngx_fd_t  fd;
    long      pid;

    if (ngx_http_cache_purge_state_path(ns, NGX_CACHE_PURGE_RUNNING_SUFFIX,
                                        path, sizeof(path) - 1)
        != NGX_OK)
    {
        return;
    }

    fd = ngx_open_file(path, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        return;
    }

    n = ngx_read_fd(fd, buf, sizeof(buf) - 1);
    (void) ngx_close_file(fd);

    if (n <= 0) {
        return;
    }

    buf[n] = '\0';
    pid = strtol((char *) buf, NULL, 10);

    /* An old master must not clear the marker of its successor. */
    if (pid != (long) owner) {
        return;
    }

    (void) ngx_delete_file(path);
    (void) ngx_http_cache_purge_sync_state_dir(ns);
}


static ngx_int_t
ngx_http_cache_purge_create_running(ngx_http_cache_purge_namespace_t *ns)
{
    u_char    path[NGX_MAX_PATH + 1];
    u_char    buf[32];
    u_char   *p;
    ngx_fd_t  fd;
    ssize_t   n;
    size_t    len;

    if (ngx_http_cache_purge_state_path(ns, NGX_CACHE_PURGE_RUNNING_SUFFIX,
                                        path, sizeof(path) - 1)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    fd = ngx_open_file(path, NGX_FILE_WRONLY|NGX_FILE_TRUNCATE,
                       NGX_FILE_CREATE_OR_OPEN, NGX_FILE_DEFAULT_ACCESS);
    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_EMERG, ns->pool->log, ngx_errno,
                      "ngx_cache_purge: cannot create \"%s\"", path);
        return NGX_ERROR;
    }

    p = ngx_snprintf(buf, sizeof(buf) - 1, "%P", ngx_pid);
    len = p - buf;

    n = ngx_write_fd(fd, buf, len);

    if (n != (ssize_t) len) {
        (void) ngx_close_file(fd);
        ngx_log_error(NGX_LOG_EMERG, ns->pool->log, ngx_errno,
                      "ngx_cache_purge: cannot write \"%s\"", path);
        return NGX_ERROR;
    }

#if !(NGX_WIN32)
    if (fsync(fd) == -1) {
        (void) ngx_close_file(fd);
        return NGX_ERROR;
    }
#endif

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        return NGX_ERROR;
    }

    return ngx_http_cache_purge_sync_state_dir(ns);
}


/*
 * Only ENOENT means the marker is absent.  Any other open error may be
 * hiding a marker that exists, so the caller must not trust the snapshot.
 */
static ngx_int_t
ngx_http_cache_purge_running_exists(ngx_http_cache_purge_namespace_t *ns)
{
    u_char    path[NGX_MAX_PATH + 1];
    ngx_fd_t  fd;

    if (ngx_http_cache_purge_state_path(ns, NGX_CACHE_PURGE_RUNNING_SUFFIX,
                                        path, sizeof(path) - 1)
        != NGX_OK)
    {
        return 1;
    }

    fd = ngx_open_file(path, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        return ngx_errno == NGX_ENOENT ? 0 : 1;
    }

    (void) ngx_close_file(fd);
    return 1;
}


static ngx_int_t
ngx_http_cache_purge_insert_rule(ngx_slab_pool_t *slab,
    ngx_http_cache_purge_generation_t *state, ngx_str_t *key,
    ngx_flag_t prefix, uint64_t generation)
{
    ngx_http_cache_purge_rule_t  *node, **link;
    size_t                        i;
    u_char                        c;

    if (key->len == 0) {
        return NGX_DECLINED;
    }

    link = prefix ? &state->prefix : &state->exact;
    node = NULL;

    for (i = 0; i < key->len; i++) {
        c = prefix ? ngx_tolower(key->data[i]) : key->data[i];

        while (*link != NULL && (*link)->byte != c) {
            link = &(*link)->sibling;
        }

        if (*link == NULL) {
            node = ngx_slab_calloc_locked(slab, sizeof(*node));
            if (node == NULL) {
                return NGX_ERROR;
            }
            node->byte = c;
            node->allocated = state->allocated;
            state->allocated = node;
            if (state->tail == NULL) {
                state->tail = node;
            }
            *link = node;
        }

        node = *link;
        link = &node->child;
    }

    if (node == NULL) {
        return NGX_DECLINED;
    }

    node->generation = generation;
    return NGX_OK;
}


static ngx_int_t
ngx_http_cache_purge_generation_restore(ngx_http_cache_purge_namespace_t *ns,
    ngx_slab_pool_t *slab)
{
    ngx_http_cache_purge_generation_t *state = ns->shared;
    ngx_file_info_t                    fi;
    ngx_fd_t                           fd;
    ngx_str_t                          key;
    uint64_t                           baseline, serial, generation;
    uint32_t                           count, kind, crc, stored;
    off_t                              file_size;
    size_t                             len, ns_len, offset;
    u_char                             path[NGX_MAX_PATH + 1];
    u_char                            *buf, *p, *end;
    ngx_int_t                          rc;

    if (ngx_http_cache_purge_state_path(ns, NGX_CACHE_PURGE_STATE_SUFFIX,
                                        path, sizeof(path) - 1)
        != NGX_OK)
    {
        return NGX_DECLINED;
    }

    if (ngx_file_info(path, &fi) == NGX_FILE_ERROR) {
        return NGX_DECLINED;
    }

    file_size = ngx_file_size(&fi);

    if (file_size < NGX_CACHE_PURGE_STATE_HEADER
        || file_size > NGX_CACHE_PURGE_STATE_MAX)
    {
        return NGX_DECLINED;
    }

    buf = ngx_alloc((size_t) file_size, ns->pool->log);
    if (buf == NULL) {
        return NGX_DECLINED;
    }

    fd = ngx_open_file(path, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        ngx_free(buf);
        return NGX_DECLINED;
    }

    offset = 0;
    while (offset < (size_t) file_size) {
        ssize_t n = ngx_read_fd(fd, buf + offset,
                                (size_t) file_size - offset);
        if (n <= 0) {
            break;
        }
        offset += (size_t) n;
    }

    (void) ngx_close_file(fd);

    if (offset != (size_t) file_size) {
        ngx_free(buf);
        return NGX_DECLINED;
    }

    if (ngx_http_cache_purge_get32(buf) != NGX_CACHE_PURGE_STATE_MAGIC
        || ngx_http_cache_purge_get32(buf + 4)
           != NGX_CACHE_PURGE_STATE_VERSION)
    {
        ngx_free(buf);
        return NGX_DECLINED;
    }

    ns_len = ngx_http_cache_purge_get32(buf + 8);
    count = ngx_http_cache_purge_get32(buf + 12);
    baseline = ngx_http_cache_purge_get64(buf + 40);
    serial = ngx_http_cache_purge_get64(buf + 48);
    stored = ngx_http_cache_purge_get32(buf + NGX_CACHE_PURGE_STATE_CRC);

    if (ns_len != ns->name.len
        || NGX_CACHE_PURGE_STATE_HEADER + ns_len > (size_t) file_size
        || ngx_memcmp(buf + NGX_CACHE_PURGE_STATE_HEADER, ns->name.data,
                      ns_len)
           != 0
        || baseline == 0 || serial < baseline)
    {
        ngx_free(buf);
        return NGX_DECLINED;
    }

    /* Recompute with the checksum field cleared, over the whole image. */
    (void) ngx_http_cache_purge_put32(buf + NGX_CACHE_PURGE_STATE_CRC, 0);
    ngx_crc32_init(crc);
    (void) ngx_crc32_update(&crc, buf, (size_t) file_size);
    ngx_crc32_final(crc);

    if (crc != stored) {
        ngx_free(buf);
        return NGX_DECLINED;
    }

    state->baseline = baseline;
    state->serial = serial;
    ngx_memcpy(state->epoch, buf + 24, 10);

    p = buf + NGX_CACHE_PURGE_STATE_HEADER + ns_len;
    end = buf + (size_t) file_size;
    rc = NGX_OK;

    ngx_shmtx_lock(&slab->mutex);

    for ( ;; ) {
        if (count-- == 0) {
            break;
        }

        if (p + 1 + 2 + 8 > end) {
            rc = NGX_DECLINED;
            break;
        }

        kind = *p++;
        len = ((size_t) p[0] << 8) | (size_t) p[1];
        p += 2;
        generation = ngx_http_cache_purge_get64(p);
        p += 8;

        if (len == 0 || p + len > end || generation == 0) {
            rc = NGX_DECLINED;
            break;
        }

        key.data = p;
        key.len = len;
        p += len;

        if (ngx_http_cache_purge_insert_rule(slab, state, &key,
                                             kind == 1, generation)
            != NGX_OK)
        {
            rc = NGX_DECLINED;
            break;
        }
    }

    if (rc == NGX_OK && p != end) {
        rc = NGX_DECLINED;
    }

    ngx_shmtx_unlock(&slab->mutex);

    if (rc != NGX_OK) {
        /* Never leave a half-restored trie behind. */
        if (state->allocated != NULL) {
            state->tail->allocated = state->garbage;
            state->garbage = state->allocated;
            state->allocated = state->tail = NULL;
        }
        state->exact = state->prefix = NULL;
        state->baseline = 1;
        state->serial = 1;
        ngx_memzero(state->epoch, 10);
        ngx_free(buf);
        return NGX_DECLINED;
    }

    ngx_free(buf);
    return NGX_OK;
}


ngx_int_t
ngx_http_cache_purge_generation_persist(ngx_http_cache_purge_namespace_t *ns)
{
    ngx_http_cache_purge_generation_t *state;
    ngx_http_cache_purge_rule_write_t  sizing, writing;
    ngx_slab_pool_t                   *slab;
    uint64_t                           serial;
    uint32_t                           crc;
    size_t                             total, written;
    u_char                             path[NGX_MAX_PATH + 1];
    u_char                             tmp[NGX_MAX_PATH + 1];
    u_char                             keybuf[NGX_CACHE_PURGE_LOGICAL_KEY_MAX + 1];
    u_char                            *buf, *p;
    ngx_fd_t                           fd;
    ngx_int_t                          rc;
    ssize_t                            n;

    if (ns->shared == NULL) {
        return NGX_OK;
    }

    state = ns->shared;
    slab = (ngx_slab_pool_t *) ns->zone->shm.addr;

    ngx_shmtx_lock(&slab->mutex);
    serial = state->serial;

    ngx_memzero(&sizing, sizeof(sizing));
    sizing.kind = 0;

    rc = ngx_http_cache_purge_rule_walk(state, state->exact, keybuf,
                                        NGX_CACHE_PURGE_LOGICAL_KEY_MAX,
                                        ngx_http_cache_purge_rule_write,
                                        &sizing, ns->pool->log);
    if (rc == NGX_OK) {
        sizing.kind = 1;
        rc = ngx_http_cache_purge_rule_walk(state, state->prefix, keybuf,
                                            NGX_CACHE_PURGE_LOGICAL_KEY_MAX,
                                            ngx_http_cache_purge_rule_write,
                                            &sizing, ns->pool->log);
    }

    if (rc != NGX_OK) {
        ngx_shmtx_unlock(&slab->mutex);
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, 0,
                      "ngx_cache_purge: cannot size the namespace snapshot "
                      "for \"%V\"", &ns->name);
        return NGX_ERROR;
    }

    total = NGX_CACHE_PURGE_STATE_HEADER + ns->name.len + sizing.size;

    if (total > NGX_CACHE_PURGE_STATE_MAX) {
        ngx_shmtx_unlock(&slab->mutex);
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, 0,
                      "ngx_cache_purge: namespace snapshot for \"%V\" is %uz "
                      "bytes, above the %uz byte limit; keeping the previous "
                      "snapshot", &ns->name, total,
                      (size_t) NGX_CACHE_PURGE_STATE_MAX);
        return NGX_ERROR;
    }

    buf = ngx_alloc(total, ns->pool->log);
    if (buf == NULL) {
        ngx_shmtx_unlock(&slab->mutex);
        return NGX_ERROR;
    }

    p = ngx_http_cache_purge_put32(buf, NGX_CACHE_PURGE_STATE_MAGIC);
    p = ngx_http_cache_purge_put32(p, NGX_CACHE_PURGE_STATE_VERSION);
    p = ngx_http_cache_purge_put32(p, (uint32_t) ns->name.len);
    p = ngx_http_cache_purge_put32(p, sizing.count);
    p = ngx_http_cache_purge_put32(p, 0);
    p = ngx_http_cache_purge_put32(p, 0);
    ngx_memcpy(p, state->epoch, 10);
    p += 10;
    ngx_memzero(p, 6);
    p += 6;
    p = ngx_http_cache_purge_put64(p, state->baseline);
    p = ngx_http_cache_purge_put64(p, state->serial);
    p = ngx_http_cache_purge_put32(p, 0);   /* checksum, filled below */
    p = ngx_http_cache_purge_put32(p, 0);
    ngx_memcpy(p, ns->name.data, ns->name.len);
    p += ns->name.len;

    ngx_memzero(&writing, sizeof(writing));
    writing.p = p;

    rc = ngx_http_cache_purge_rule_walk(state, state->exact, keybuf,
                                        NGX_CACHE_PURGE_LOGICAL_KEY_MAX,
                                        ngx_http_cache_purge_rule_write,
                                        &writing, ns->pool->log);
    if (rc == NGX_OK) {
        writing.kind = 1;
        rc = ngx_http_cache_purge_rule_walk(state, state->prefix, keybuf,
                                            NGX_CACHE_PURGE_LOGICAL_KEY_MAX,
                                            ngx_http_cache_purge_rule_write,
                                            &writing, ns->pool->log);
    }

    ngx_shmtx_unlock(&slab->mutex);

    if (rc != NGX_OK || (size_t) (writing.p - buf) != total) {
        ngx_free(buf);
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, 0,
                      "ngx_cache_purge: cannot serialize the namespace "
                      "snapshot for \"%V\"", &ns->name);
        return NGX_ERROR;
    }

    ngx_crc32_init(crc);
    (void) ngx_crc32_update(&crc, buf, (size_t) (writing.p - buf));
    ngx_crc32_final(crc);
    (void) ngx_http_cache_purge_put32(buf + NGX_CACHE_PURGE_STATE_CRC, crc);

    if (ngx_http_cache_purge_state_path(ns, NGX_CACHE_PURGE_STATE_SUFFIX,
                                        path, sizeof(path) - 1)
        != NGX_OK
        || ngx_http_cache_purge_state_path(ns, NGX_CACHE_PURGE_TMP_SUFFIX,
                                           tmp, sizeof(tmp) - 1)
        != NGX_OK)
    {
        ngx_free(buf);
        return NGX_ERROR;
    }

    fd = ngx_open_file(tmp, NGX_FILE_WRONLY|NGX_FILE_TRUNCATE,
                       NGX_FILE_CREATE_OR_OPEN, NGX_FILE_DEFAULT_ACCESS);
    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, ngx_errno,
                      "ngx_cache_purge: cannot create \"%s\"", tmp);
        ngx_free(buf);
        return NGX_ERROR;
    }

    written = 0;
    rc = NGX_OK;

    while (written < total) {
        n = ngx_write_fd(fd, buf + written, total - written);
        if (n <= 0) {
            rc = NGX_ERROR;
            break;
        }
        written += (size_t) n;
    }

    if (rc == NGX_OK) {
#if !(NGX_WIN32)
        if (fsync(fd) == -1) {
            rc = NGX_ERROR;
        }
#endif
    }

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        rc = NGX_ERROR;
    }

    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, ngx_errno,
                      "ngx_cache_purge: cannot write \"%s\"", tmp);
        (void) ngx_delete_file(tmp);
        ngx_free(buf);
        return NGX_ERROR;
    }

    ngx_free(buf);

    ngx_shmtx_lock(&slab->mutex);

    if (state->serial != serial) {
        /* A newer invalidation owns the snapshot. */
        ngx_shmtx_unlock(&slab->mutex);
        (void) ngx_delete_file(tmp);
        return NGX_OK;
    }

    rc = ngx_rename_file(tmp, path) == 0 ? NGX_OK : NGX_ERROR;

    ngx_shmtx_unlock(&slab->mutex);

    if (rc != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ns->pool->log, ngx_errno,
                      "ngx_cache_purge: cannot replace \"%s\"", path);
        (void) ngx_delete_file(tmp);
        return NGX_ERROR;
    }

    return ngx_http_cache_purge_sync_state_dir(ns);
}


static ngx_int_t
ngx_http_cache_purge_generation_init(ngx_shm_zone_t *zone, void *data)
{
    ngx_http_cache_purge_namespace_t *old = data, *ns = zone->data;
    ngx_http_cache_purge_generation_t *state;
    ngx_slab_pool_t                    *slab;
    ngx_file_t                         file;

    slab = (ngx_slab_pool_t *) zone->shm.addr;
    if (old != NULL || zone->shm.exists) {
        state = old != NULL ? old->shared : slab->data;
        if (state == NULL) {
            ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                          "ngx_cache_purge: namespace \"%V\" has no "
                          "initialized "
                          "shared state; restart nginx", &ns->name);
            return NGX_ERROR;
        }
        if (state->version != NGX_CACHE_PURGE_NAMESPACE_VERSION) {
            ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                          "ngx_cache_purge: namespace \"%V\" has shared state "
                          "version %ui, expected %d; restart nginx",
                          &ns->name, state->version,
                          NGX_CACHE_PURGE_NAMESPACE_VERSION);
            return NGX_ERROR;
        }
        if (state->state_path.len != ns->state_path->name.len
            || ngx_memcmp(state->state_path.data, ns->state_path->name.data,
                          state->state_path.len))
        {
            ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                          "ngx_cache_purge: namespace \"%V\" changes state "
                          "path from \"%V\" to \"%V\"; restart nginx",
                          &ns->name, &state->state_path, &ns->state_path->name);
            return NGX_ERROR;
        }
        ns->shared = state;
        return NGX_OK;
    }

    state = ngx_slab_calloc(slab, sizeof(*state) + ns->state_path->name.len);
    if (state == NULL) {
        return NGX_ERROR;
    }
    state->version = NGX_CACHE_PURGE_NAMESPACE_VERSION;
    state->serial = state->baseline = 1;
    state->state_path.len = ns->state_path->name.len;
    state->state_path.data = (u_char *) (state + 1);
    ngx_memcpy(state->state_path.data, ns->state_path->name.data,
               state->state_path.len);

    slab->data = state;
    ns->shared = state;

    /*
     * Restore only when the previous run reached its shutdown hook.  A
     * leftover running marker means invalidation rules may not have been
     * committed, so the epoch must rotate instead: a file whose rule was
     * lost would otherwise become reachable again.
     */
    if (!ngx_http_cache_purge_running_exists(ns)
        && ngx_http_cache_purge_generation_restore(ns, slab) == NGX_OK)
    {
        /*
         * The marker must exist before any invalidation is accepted;
         * otherwise a later crash would let the next start trust this
         * snapshot and resurrect invalidated entries.
         */
        if (ngx_http_cache_purge_create_running(ns) != NGX_OK) {
            return NGX_ERROR;
        }
        return NGX_OK;
    }

    /* Persistent O_EXCL markers prevent epoch reuse after a full restart.
     * The configured state directory must be retained with its cache. */
    ngx_memzero(&file, sizeof(file));
    file.fd = NGX_INVALID_FILE;
    file.log = zone->shm.log;
    if (ngx_create_temp_file(&file, ns->state_path, ns->pool, 1, 0,
                             NGX_FILE_OWNER_ACCESS) != NGX_OK)
    {
        return NGX_ERROR;
    }
    ngx_memcpy(state->epoch, file.name.data + file.name.len - 10, 10);
    ngx_pool_run_cleanup_file(ns->pool, file.fd);

    if (ngx_http_cache_purge_create_running(ns) != NGX_OK) {
        return NGX_ERROR;
    }
    return NGX_OK;
}


ngx_http_cache_purge_namespace_t *
ngx_http_cache_purge_namespace_create(ngx_conf_t *cf, ngx_str_t *name,
    size_t size, const ngx_str_t *state_path)
{
    ngx_http_cache_purge_namespace_t  *ns;
    ngx_str_t                         zone_name;
    ngx_list_part_t                  *part;
    size_t                            i;
    u_char                            c;

    if (name->len == 0 || name->len > 32 || size < 8 * ngx_pagesize) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "ngx_cache_purge: namespace \"%V\" has name length "
                          "%uz and shared memory size %uz bytes; expected "
                          "1 to 32 name bytes and at least %uz shared bytes",
                          name, name->len, size, (size_t) (8 * ngx_pagesize));
        return NULL;
    }
    for (i = 0; i < name->len; i++) {
        c = name->data[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')
            && !(c >= '0' && c <= '9') && c != '_' && c != '-')
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                              "ngx_cache_purge: invalid namespace name "
                              "\"%V\"; "
                              "expected ASCII letters, digits, \"_\" or \"-\"",
                              name);
            return NULL;
        }
    }
    ns = ngx_pcalloc(cf->pool, sizeof(*ns));
    if (ns == NULL) {
        return NULL;
    }
    ns->name = *name;
    ns->pool = cf->pool;
    ns->state_path = ngx_pcalloc(cf->pool, sizeof(ngx_path_t));
    if (ns->state_path == NULL) {
        return NULL;
    }
    ns->state_path->name = *state_path;
    if (ngx_conf_full_name(cf->cycle, &ns->state_path->name, 0) != NGX_OK
        || ngx_add_path(cf, &ns->state_path) != NGX_OK)
    {
        return NULL;
    }
    zone_name.len = sizeof("cache_purge_namespace:") - 1 + name->len;
    zone_name.data = ngx_pnalloc(cf->pool, zone_name.len);
    if (zone_name.data == NULL) {
        return NULL;
    }
    ngx_sprintf(zone_name.data, "cache_purge_namespace:%V", name);
    if (cf->cycle->old_cycle != NULL) {
        part = &cf->cycle->old_cycle->shared_memory.part;
        for ( ;; part = part->next) {
            ngx_shm_zone_t  *zones = part->elts;
            for (i = 0; i < part->nelts; i++) {
                if (zones[i].tag == &ngx_http_cache_purge_module
                    && zones[i].shm.name.len == zone_name.len
                    && ngx_memcmp(zones[i].shm.name.data, zone_name.data,
                                  zone_name.len) == 0
                    && zones[i].shm.size != size)
                {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "ngx_cache_purge: namespace \"%V\" changes shared "
                        "memory size from %uz to %uz bytes; restart nginx",
                        name, zones[i].shm.size, size);
                    return NULL;
                }
            }
            if (part->next == NULL) {
                break;
            }
        }
    }
    ns->zone = ngx_shared_memory_add(cf, &zone_name, size,
                                    &ngx_http_cache_purge_module);
    if (ns->zone == NULL) {
        return NULL;
    }
    if (ns->zone->data != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                          "ngx_cache_purge: duplicate namespace \"%V\"", name);
        return NULL;
    }
    ns->zone->data = ns;
    ns->zone->init = ngx_http_cache_purge_generation_init;
    return ns;
}


/* Byte tries have at most 256 siblings per level, independent of cache size. */
static uint64_t
ngx_http_cache_purge_generation_lookup(ngx_http_cache_purge_generation_t *state,
    ngx_str_t *key)
{
    ngx_http_cache_purge_rule_t  *node;
    uint64_t                     generation;
    size_t                       i;
    ngx_uint_t                   prefix;
    u_char                       c;

    generation = state->baseline;
    for (prefix = 0; prefix != 2; prefix++) {
        node = prefix ? state->prefix : state->exact;
        for (i = 0; i < key->len && node != NULL; i++) {
            c = prefix ? ngx_tolower(key->data[i]) : key->data[i];
            while (node != NULL && node->byte != c) {
                node = node->sibling;
            }
            if (node == NULL) {
                break;
            }
            if ((prefix || i + 1 == key->len)
                && node->generation > generation)
            {
                generation = node->generation;
            }
            node = node->child;
        }
    }
    return generation;
}


/* Reset in constant time; reclaim detached nodes in bounded subsequent calls. */
static void
ngx_http_cache_purge_generation_reset(ngx_http_cache_purge_generation_t *state)
{
    state->baseline = state->serial;
    state->exact = state->prefix = NULL;
    if (state->allocated != NULL) {
        state->tail->allocated = state->garbage;
        state->garbage = state->allocated;
        state->allocated = state->tail = NULL;
    }
}


static ngx_int_t
ngx_http_cache_purge_generation_decode(ngx_http_cache_purge_namespace_t *ns,
    ngx_str_t *key, ngx_str_t *logical, u_char **epoch, uint64_t *generation)
{
    u_char          *p;
    size_t           i, offset;
    ngx_int_t        digit;

    offset = 3 + ns->name.len + 1 + 10 + 1 + 16 + 1;
    if (key->len < offset || ngx_memcmp(key->data, "p4:", 3)
        || ngx_memcmp(key->data + 3, ns->name.data, ns->name.len)
        || key->data[3 + ns->name.len] != ':')
    {
        return NGX_DECLINED;
    }
    *epoch = key->data + 3 + ns->name.len + 1;
    if ((*epoch)[10] != ':' || (*epoch)[27] != ':') {
        return NGX_DECLINED;
    }
    for (i = 0; i < 10; i++) {
        if ((*epoch)[i] < '0' || (*epoch)[i] > '9') {
            return NGX_DECLINED;
        }
    }
    p = *epoch + 11;
    *generation = 0;
    for (i = 0; i < 16; i++) {
        digit = ngx_hextoi(p + i, 1);
        if (digit == NGX_ERROR) {
            return NGX_DECLINED;
        }
        *generation = (*generation << 4) | (uint64_t) digit;
    }
    logical->data = key->data + offset;
    logical->len = key->len - offset;
    return NGX_OK;
}


ngx_int_t
ngx_http_cache_purge_generation_invalidate(ngx_http_cache_purge_namespace_t *ns,
    ngx_str_t *key, ngx_flag_t all, ngx_str_t *expected)
{
    ngx_http_cache_purge_generation_t  *state = ns->shared;
    ngx_http_cache_purge_rule_t        *node, **link;
    ngx_slab_pool_t                    *slab;
    ngx_uint_t                         n;
    size_t                             i, len;
    ngx_flag_t                         prefix;
    u_char                             c, *epoch;
    ngx_str_t                          logical;
    uint64_t                           generation;

    if (key->len > NGX_CACHE_PURGE_LOGICAL_KEY_MAX) {
        return NGX_ERROR;
    }
    slab = (ngx_slab_pool_t *) ns->zone->shm.addr;
    /* Admission holds its own lock. This nested acquisition never waits;
     * no code acquires the admission lock while holding a namespace lock. */
    if (!ngx_shmtx_trylock(&slab->mutex)) {
        return NGX_AGAIN;
    }
    if (expected != NULL
        && (ngx_http_cache_purge_generation_decode(ns, expected, &logical,
                                                   &epoch, &generation) != NGX_OK
            || logical.len != key->len
            || ngx_memcmp(logical.data, key->data, key->len)
            || ngx_memcmp(epoch, state->epoch, 10)
            || generation != ngx_http_cache_purge_generation_lookup(state, key)))
    {
        ngx_shmtx_unlock(&slab->mutex);
        return NGX_DECLINED;
    }
    if (state->serial == (uint64_t) -1) {
        ngx_shmtx_unlock(&slab->mutex);
        return NGX_ERROR;
    }
    for (n = 0; n < 64 && state->garbage != NULL; n++) {
        node = state->garbage;
        state->garbage = node->allocated;
        ngx_slab_free_locked(slab, node);
    }
    state->serial++;
    len = key->len;
    prefix = expected == NULL && len != 0 && key->data[len - 1] == '*';
    if (prefix) {
        len--;
    }
    if (all || len == 0) {
        ngx_http_cache_purge_generation_reset(state);
        ngx_shmtx_unlock(&slab->mutex);
        return NGX_OK;
    }
    link = prefix ? &state->prefix : &state->exact;
    /* Empty selectors returned above; at least one trie node is visited. */
    i = 0;
    do {
        c = prefix ? ngx_tolower(key->data[i]) : key->data[i];
        while (*link != NULL && (*link)->byte != c) {
            link = &(*link)->sibling;
        }
        if (*link == NULL) {
            node = ngx_slab_calloc_locked(slab, sizeof(*node));
            if (node == NULL) {
                /* Bounded memory: pressure broadens invalidation to the
                 * namespace; it never makes an invalidated key reachable. */
                ngx_http_cache_purge_generation_reset(state);
                ngx_shmtx_unlock(&slab->mutex);
                return NGX_OK;
            }
            node->byte = c;
            node->allocated = state->allocated;
            state->allocated = node;
            if (state->tail == NULL) {
                state->tail = node;
            }
            *link = node;
        }
        node = *link;
        link = &node->child;
    } while (++i < len);
    node->generation = state->serial;
    ngx_shmtx_unlock(&slab->mutex);
    return NGX_OK;
}


static void
ngx_http_cache_purge_generation_cleanup(void *data)
{
    (void) data;
}


ngx_int_t
ngx_http_cache_purge_generation_key(ngx_http_request_t *r,
    ngx_http_cache_purge_namespace_t *ns, ngx_http_complex_value_t *expression,
    ngx_str_t *logical, ngx_str_t *encoded)
{
    ngx_pool_cleanup_t                        *cln;
    ngx_http_cache_purge_generation_request_t *ctx;
    ngx_http_cache_purge_snapshot_t           *snap;
    ngx_slab_pool_t                           *slab;
    ngx_str_t                                 key;
    uint64_t                                  generation;
    u_char                                   *p;
    ngx_int_t                                 rc;

    ctx = NULL;
    for (cln = r->pool->cleanup; cln != NULL; cln = cln->next) {
        if (cln->handler == ngx_http_cache_purge_generation_cleanup) {
            ctx = cln->data;
            if (ctx->request == r) {
                break;
            }
            ctx = NULL;
        }
    }
    if (ctx == NULL) {
        cln = ngx_pool_cleanup_add(r->pool, sizeof(*ctx));
        if (cln == NULL) {
            return NGX_ERROR;
        }
        ctx = cln->data;
        ngx_memzero(ctx, sizeof(*ctx));
        ctx->request = r;
        cln->handler = ngx_http_cache_purge_generation_cleanup;
    }
    if (ctx->evaluating) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_cache_purge: \"cache_purge_key\" for namespace "
                      "\"%V\" recursively refers to itself", &ns->name);
        return NGX_ERROR;
    }
    ctx->evaluating = 1;
    rc = ngx_http_complex_value(r, expression, &key);
    ctx->evaluating = 0;
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }
    if (key.len > NGX_CACHE_PURGE_LOGICAL_KEY_MAX) {
        return NGX_DECLINED;
    }
    for (snap = ctx->snapshots; snap != NULL; snap = snap->next) {
        if (snap->ns == ns && snap->logical.len == key.len
            && ngx_memcmp(snap->logical.data, key.data, key.len) == 0)
        {
            *logical = snap->logical;
            *encoded = snap->encoded;
            return NGX_OK;
        }
    }
    snap = ngx_pcalloc(r->pool, sizeof(*snap));
    if (snap == NULL) {
        return NGX_ERROR;
    }
    snap->encoded.data = ngx_pnalloc(r->pool, key.len + ns->name.len + 32);
    if (snap->encoded.data == NULL) {
        return NGX_ERROR;
    }
    slab = (ngx_slab_pool_t *) ns->zone->shm.addr;
    ngx_shmtx_lock(&slab->mutex);
    generation = ngx_http_cache_purge_generation_lookup(ns->shared, &key);
    p = ngx_sprintf(snap->encoded.data, "p4:%V:%*s:%016xL:", &ns->name,
                    (size_t) 10, ns->shared->epoch, generation);
    ngx_shmtx_unlock(&slab->mutex);
    snap->logical.data = p;
    snap->logical.len = key.len;
    p = ngx_cpymem(p, key.data, key.len);
    snap->encoded.len = p - snap->encoded.data;
    snap->ns = ns;
    snap->next = ctx->snapshots;
    ctx->snapshots = snap;
    *logical = snap->logical;
    *encoded = snap->encoded;
    return NGX_OK;
}


ngx_int_t
ngx_http_cache_purge_generation_stale(ngx_http_cache_purge_namespace_t *ns,
    ngx_str_t *key)
{
    ngx_slab_pool_t *slab;
    ngx_str_t logical;
    u_char *epoch;
    uint64_t generation, current;
    ngx_flag_t stale;

    if (ngx_http_cache_purge_generation_decode(ns, key, &logical, &epoch,
                                               &generation) != NGX_OK)
    {
        return NGX_DECLINED;
    }
    slab = (ngx_slab_pool_t *) ns->zone->shm.addr;
    ngx_shmtx_lock(&slab->mutex);
    current = ngx_http_cache_purge_generation_lookup(ns->shared, &logical);
    stale = ngx_memcmp(epoch, ns->shared->epoch, 10) != 0 || generation < current;
    ngx_shmtx_unlock(&slab->mutex);
    return stale ? NGX_OK : NGX_DECLINED;
}


/* Everything an external file scanner needs to apply the same predicate as
 * ngx_http_cache_purge_generation_stale() to files it reads from disk, except
 * the per-key rules. A file is stale when its epoch differs from "epoch" or its
 * generation is below "baseline". When "rules" is false the live generation of
 * every key equals "baseline", so any other file is live. When "rules" is true
 * a file at or above "baseline" may still be stale through an exact or prefix
 * rule. One short critical section, the same lock the request path takes. */
void
ngx_http_cache_purge_generation_view(ngx_http_cache_purge_namespace_t *ns,
    ngx_http_cache_purge_generation_view_t *view)
{
    ngx_slab_pool_t  *slab;

    slab = (ngx_slab_pool_t *) ns->zone->shm.addr;
    ngx_shmtx_lock(&slab->mutex);
    ngx_memcpy(view->epoch, ns->shared->epoch, 10);
    view->serial = ns->shared->serial;
    view->baseline = ns->shared->baseline;
    view->rules = ns->shared->exact != NULL || ns->shared->prefix != NULL;
    ngx_shmtx_unlock(&slab->mutex);
}


ngx_int_t
ngx_http_cache_purge_generation_current(ngx_http_cache_purge_namespace_t *ns,
    ngx_str_t *key, ngx_str_t *logical)
{
    ngx_slab_pool_t *slab;
    u_char *epoch;
    uint64_t generation, current;
    ngx_flag_t matches;

    if (ngx_http_cache_purge_generation_decode(ns, key, logical, &epoch,
                                               &generation) != NGX_OK)
    {
        return NGX_DECLINED;
    }
    slab = (ngx_slab_pool_t *) ns->zone->shm.addr;
    ngx_shmtx_lock(&slab->mutex);
    current = ngx_http_cache_purge_generation_lookup(ns->shared, logical);
    matches = ngx_memcmp(epoch, ns->shared->epoch, 10) == 0 && generation == current;
    ngx_shmtx_unlock(&slab->mutex);
    return matches ? NGX_OK : NGX_DECLINED;
}


#endif
