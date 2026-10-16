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
        if (state->version != 1) {
            ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                          "ngx_cache_purge: namespace \"%V\" has shared state "
                          "version %ui, expected 1; restart nginx",
                          &ns->name, state->version);
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
    state->version = 1;
    state->serial = state->baseline = 1;
    state->state_path.len = ns->state_path->name.len;
    state->state_path.data = (u_char *) (state + 1);
    ngx_memcpy(state->state_path.data, ns->state_path->name.data,
               state->state_path.len);

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
    slab->data = state;
    ns->shared = state;
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
