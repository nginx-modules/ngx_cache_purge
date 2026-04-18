#include <ngx_config.h>
#include <nginx.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "ngx_cache_pilot_tag.h"
#include "ngx_cache_pilot_metrics.h"

#if (NGX_HTTP_CACHE)

#define NGX_CACHE_PILOT_TAG_INDEX_STATE_DISABLED   0
#define NGX_CACHE_PILOT_TAG_INDEX_STATE_CONFIGURED 1
#define NGX_CACHE_PILOT_TAG_INDEX_STATE_READY      2

/* ── Per-zone snapshot collected before serialization ── */
typedef struct {
    ngx_str_t  name;
    off_t      size;
    off_t      max_size;
    ngx_uint_t cold;             /* 1 while nginx cache loader is running */
    ngx_uint_t entries_valid;
    ngx_uint_t entries_expired;
    ngx_uint_t entries_updating;
    ngx_uint_t tag_index_state;
    ngx_uint_t has_tag_index;
    ngx_uint_t tag_backend;      /* NGX_HTTP_CACHE_TAG_BACKEND_* */
    ngx_uint_t queue_dropped;
    ngx_uint_t queue_size;
    ngx_uint_t queue_capacity;
} ngx_http_cache_pilot_zone_snapshot_t;


/* ── rbtree walk (called while holding cache->shpool->mutex) ── */

static void
ngx_http_cache_pilot_walk_rbtree(ngx_rbtree_node_t *node,
                                 ngx_rbtree_node_t *sentinel, time_t now,
                                 ngx_uint_t *valid, ngx_uint_t *expired, ngx_uint_t *updating) {
    ngx_http_file_cache_node_t *fcn;

    if (node == sentinel) {
        return;
    }

    ngx_http_cache_pilot_walk_rbtree(node->left,  sentinel, now,
                                     valid, expired, updating);
    ngx_http_cache_pilot_walk_rbtree(node->right, sentinel, now,
                                     valid, expired, updating);

    fcn = (ngx_http_file_cache_node_t *) node;

    if (!fcn->exists) {
        return;
    }

#if (nginx_version >= 8001) \
    || ((nginx_version < 8000) && (nginx_version >= 7060))
    if (fcn->updating) {
        (*updating)++;
        return;
    }
#endif

    if (fcn->valid_sec > now) {
        (*valid)++;
    } else {
        (*expired)++;
    }
}


/* ── Collect one zone snapshot ── */

static void
ngx_http_cache_pilot_snapshot_zone(ngx_http_cache_pilot_stat_zone_t *sz,
                                   ngx_http_cache_pilot_main_conf_t *pmcf,
                                   ngx_http_cache_pilot_zone_snapshot_t *snap) {
    ngx_http_file_cache_t *cache;
#if (NGX_LINUX)
    ngx_http_cache_tag_queue_ctx_t *qctx;
#endif

    cache = sz->cache;
    snap->name     = sz->name;
    snap->max_size = cache->max_size;

    /* Snapshot size + rbtree under the shpool mutex */
    ngx_shmtx_lock(&cache->shpool->mutex);

    snap->cold             = (ngx_uint_t) cache->sh->cold;
    snap->size             = cache->sh->size;
    snap->entries_valid    = 0;
    snap->entries_expired  = 0;
    snap->entries_updating = 0;

    ngx_http_cache_pilot_walk_rbtree(
        cache->sh->rbtree.root,
        cache->sh->rbtree.sentinel,
        ngx_time(),
        &snap->entries_valid,
        &snap->entries_expired,
        &snap->entries_updating);

    ngx_shmtx_unlock(&cache->shpool->mutex);

    snap->tag_index_state = NGX_CACHE_PILOT_TAG_INDEX_STATE_DISABLED;
    snap->has_tag_index  = 0;
    snap->tag_backend    = 0;
    snap->queue_dropped  = 0;
    snap->queue_size     = 0;
    snap->queue_capacity = 0;

#if (NGX_LINUX)
    if (ngx_http_cache_tag_store_configured(pmcf)) {
        snap->tag_index_state = NGX_CACHE_PILOT_TAG_INDEX_STATE_CONFIGURED;
        snap->has_tag_index = 1;
        snap->tag_backend   = (ngx_uint_t) pmcf->backend;

        if (ngx_http_cache_tag_zone_bootstrap_complete(cache)) {
            snap->tag_index_state = NGX_CACHE_PILOT_TAG_INDEX_STATE_READY;
        }

        if (pmcf->queue_zone != NULL && pmcf->queue_zone->data != NULL) {
            qctx = pmcf->queue_zone->data;
            if (qctx->sh != NULL) {
                ngx_shmtx_lock(&qctx->shpool->mutex);
                snap->queue_dropped  = (ngx_uint_t) qctx->sh->dropped;
                snap->queue_size     = (ngx_uint_t) qctx->sh->count;
                snap->queue_capacity = (ngx_uint_t) qctx->sh->capacity;
                ngx_shmtx_unlock(&qctx->shpool->mutex);
            }
        }
    }
#endif
}


/* ── Atomic counter read ── */

static ngx_inline ngx_atomic_uint_t
ngx_cache_pilot_metrics_read(ngx_atomic_t *p) {
    return (ngx_atomic_uint_t) ngx_atomic_fetch_add(p, 0);
}


/* ── Format negotiation ── */

static ngx_int_t
ngx_http_cache_pilot_negotiate_format(ngx_http_request_t *r) {
    ngx_str_t        fmt;
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t       i;

    static const u_char accept_hdr[] = "accept";

    /* ?format=json|prometheus takes precedence */
    if (ngx_http_arg(r, (u_char *) "format", 6, &fmt) == NGX_OK) {
        if (fmt.len == 10
                && ngx_strncasecmp(fmt.data, (u_char *) "prometheus", 10) == 0) {
            return NGX_CACHE_PILOT_METRICS_FORMAT_PROMETHEUS;
        }
        return NGX_CACHE_PILOT_METRICS_FORMAT_JSON;
    }

    /* Accept header fallback — iterate headers_in list (portable across all
     * nginx builds regardless of NGX_HTTP_HEADERS). */
    part = &r->headers_in.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].key.len != sizeof(accept_hdr) - 1) {
            continue;
        }

        if (ngx_strncasecmp(h[i].key.data, (u_char *) accept_hdr,
                            sizeof(accept_hdr) - 1) != 0) {
            continue;
        }

        if (ngx_strnstr(h[i].value.data, "text/plain", h[i].value.len) != NULL
                || ngx_strnstr(h[i].value.data, "application/openmetrics-text",
                               h[i].value.len) != NULL) {
            return NGX_CACHE_PILOT_METRICS_FORMAT_PROMETHEUS;
        }

        break;
    }

    return NGX_CACHE_PILOT_METRICS_FORMAT_JSON;
}


/* ── Backend name helper ── */

static const char *
ngx_http_cache_pilot_backend_str(ngx_uint_t backend) {
    switch (backend) {
    case NGX_HTTP_CACHE_TAG_BACKEND_SQLITE:
        return "sqlite";
    case NGX_HTTP_CACHE_TAG_BACKEND_REDIS:
        return "redis";
    default:
        return "unknown";
    }
}

static const char *
ngx_http_cache_pilot_tag_index_state_str(ngx_uint_t state) {
    switch (state) {
    case NGX_CACHE_PILOT_TAG_INDEX_STATE_CONFIGURED:
        return "configured";
    case NGX_CACHE_PILOT_TAG_INDEX_STATE_READY:
        return "ready";
    default:
        return "disabled";
    }
}


/* ── JSON serializer ── */

static u_char *
ngx_http_cache_pilot_write_json(u_char *p, u_char *last,
                                ngx_uint_t nzones, ngx_http_cache_pilot_zone_snapshot_t *snaps,
                                ngx_http_cache_pilot_metrics_shctx_t *m) {
    ngx_uint_t  i;
    ngx_http_cache_pilot_zone_snapshot_t *s;

    p = ngx_slprintf(p, last,
                     "{"
                     "\"version\":1,"
                     "\"timestamp\":%T,"
                     "\"purges\":{"
                     "\"exact\":{\"hard\":%uA,\"soft\":%uA},"
                     "\"wildcard\":{\"hard\":%uA,\"soft\":%uA},"
                     "\"tag\":{\"hard\":%uA,\"soft\":%uA},"
                     "\"all\":{\"hard\":%uA,\"soft\":%uA}"
                     "},"
                     "\"key_index\":{"
                     "\"exact_fanout\":%uA,"
                     "\"wildcard_hits\":%uA"
                     "},",
                     ngx_time(),
                     m ? ngx_cache_pilot_metrics_read(&m->purges_exact_hard)    : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_exact_soft)    : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_wildcard_hard) : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_wildcard_soft) : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_tag_hard)      : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_tag_soft)      : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_all_hard)      : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_all_soft)      : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->key_index_exact_fanout)  : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->key_index_wildcard_hits) : (ngx_atomic_uint_t)0);

    p = ngx_slprintf(p, last, "\"zones\":{");

    for (i = 0; i < nzones; i++) {
        s = &snaps[i];

        if (i > 0) {
            if (p < last) {
                *p++ = ',';
            }
        }

        p = ngx_slprintf(p, last,
                         "\"%V\":{"
                         "\"size\":%O,"
                         "\"max_size\":%O,"
                         "\"cold\":%s,"
                         "\"entries\":{"
                         "\"total\":%ui,"
                         "\"valid\":%ui,"
                         "\"expired\":%ui,"
                         "\"updating\":%ui"
                         "}",
                         &s->name,
                         s->size,
                         s->max_size,
                         s->cold ? "true" : "false",
                         s->entries_valid + s->entries_expired + s->entries_updating,
                         s->entries_valid,
                         s->entries_expired,
                         s->entries_updating);

        if (s->has_tag_index) {
            p = ngx_slprintf(p, last,
                             ",\"tag_index\":{"
                             "\"state\":\"%s\","
                             "\"state_code\":%ui,"
                             "\"backend\":\"%s\","
                             "\"queue\":{"
                             "\"size\":%ui,"
                             "\"capacity\":%ui,"
                             "\"dropped\":%ui"
                             "}"
                             "}",
                             ngx_http_cache_pilot_tag_index_state_str(s->tag_index_state),
                             s->tag_index_state,
                             ngx_http_cache_pilot_backend_str(s->tag_backend),
                             s->queue_size,
                             s->queue_capacity,
                             s->queue_dropped);
        }

        if (p < last) {
            *p++ = '}';
        }
    }

    p = ngx_slprintf(p, last, "}}");

    return p;
}


/* ── Prometheus serializer ── */

static u_char *
ngx_http_cache_pilot_write_prometheus(u_char *p, u_char *last,
                                      ngx_uint_t nzones, ngx_http_cache_pilot_zone_snapshot_t *snaps,
                                      ngx_http_cache_pilot_metrics_shctx_t *m) {
    ngx_uint_t  i;
    ngx_http_cache_pilot_zone_snapshot_t *s;

    /* Global purge counters */
    p = ngx_slprintf(p, last,
                     "# HELP nginx_cache_pilot_purges_total"
                     " Total cache purge operations\n"
                     "# TYPE nginx_cache_pilot_purges_total counter\n"
                     "nginx_cache_pilot_purges_total{type=\"exact\",mode=\"hard\"} %uA\n"
                     "nginx_cache_pilot_purges_total{type=\"exact\",mode=\"soft\"} %uA\n"
                     "nginx_cache_pilot_purges_total{type=\"wildcard\",mode=\"hard\"} %uA\n"
                     "nginx_cache_pilot_purges_total{type=\"wildcard\",mode=\"soft\"} %uA\n"
                     "nginx_cache_pilot_purges_total{type=\"tag\",mode=\"hard\"} %uA\n"
                     "nginx_cache_pilot_purges_total{type=\"tag\",mode=\"soft\"} %uA\n"
                     "nginx_cache_pilot_purges_total{type=\"all\",mode=\"hard\"} %uA\n"
                     "nginx_cache_pilot_purges_total{type=\"all\",mode=\"soft\"} %uA\n",
                     m ? ngx_cache_pilot_metrics_read(&m->purges_exact_hard)    : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_exact_soft)    : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_wildcard_hard) : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_wildcard_soft) : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_tag_hard)      : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_tag_soft)      : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_all_hard)      : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->purges_all_soft)      : (ngx_atomic_uint_t)0);

    /* Key-index efficiency counters */
    p = ngx_slprintf(p, last,
                     "# HELP nginx_cache_pilot_key_index_total"
                     " Cache purges served via the key index (avoids filesystem walk)\n"
                     "# TYPE nginx_cache_pilot_key_index_total counter\n"
                     "nginx_cache_pilot_key_index_total{type=\"exact_fanout\"} %uA\n"
                     "nginx_cache_pilot_key_index_total{type=\"wildcard_hits\"} %uA\n",
                     m ? ngx_cache_pilot_metrics_read(&m->key_index_exact_fanout)  : (ngx_atomic_uint_t)0,
                     m ? ngx_cache_pilot_metrics_read(&m->key_index_wildcard_hits) : (ngx_atomic_uint_t)0);

    /* Zone size */
    p = ngx_slprintf(p, last,
                     "# HELP nginx_cache_pilot_zone_size_bytes"
                     " Current cache zone size in bytes\n"
                     "# TYPE nginx_cache_pilot_zone_size_bytes gauge\n");
    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_zone_size_bytes{zone=\"%V\"} %O\n",
                         &s->name, s->size);
    }

    /* Zone max_size */
    p = ngx_slprintf(p, last,
                     "# HELP nginx_cache_pilot_zone_max_size_bytes"
                     " Configured maximum cache zone size in bytes\n"
                     "# TYPE nginx_cache_pilot_zone_max_size_bytes gauge\n");
    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_zone_max_size_bytes{zone=\"%V\"} %O\n",
                         &s->name, s->max_size);
    }

    /* Zone cold */
    p = ngx_slprintf(p, last,
                     "# HELP nginx_cache_pilot_zone_cold"
                     " 1 if the cache zone loader has not finished, 0 if warm\n"
                     "# TYPE nginx_cache_pilot_zone_cold gauge\n");
    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_zone_cold{zone=\"%V\"} %ui\n",
                         &s->name, s->cold);
    }

    /* Zone entries */
    p = ngx_slprintf(p, last,
                     "# HELP nginx_cache_pilot_zone_entries"
                     " Number of entries in the cache zone by state\n"
                     "# TYPE nginx_cache_pilot_zone_entries gauge\n");
    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_zone_entries{zone=\"%V\",state=\"valid\"} %ui\n"
                         "nginx_cache_pilot_zone_entries{zone=\"%V\",state=\"expired\"} %ui\n"
                         "nginx_cache_pilot_zone_entries{zone=\"%V\",state=\"updating\"} %ui\n",
                         &s->name, s->entries_valid,
                         &s->name, s->entries_expired,
                         &s->name, s->entries_updating);
    }

    /* Tag index metrics */
    p = ngx_slprintf(p, last,
                     "# HELP nginx_cache_pilot_tag_index_state"
                     " Per-zone tag index state: 0=disabled, 1=configured, 2=ready\n"
                     "# TYPE nginx_cache_pilot_tag_index_state gauge\n");
    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_tag_index_state{zone=\"%V\",state=\"%s\"} %ui\n",
                         &s->name,
                         ngx_http_cache_pilot_tag_index_state_str(s->tag_index_state),
                         s->tag_index_state);
    }

    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        if (!s->has_tag_index) {
            continue;
        }

        if (i == 0 || !snaps[i - 1].has_tag_index) {
            p = ngx_slprintf(p, last,
                             "# HELP nginx_cache_pilot_tag_index_info"
                             " Static metadata about the configured tag index backend (always 1)\n"
                             "# TYPE nginx_cache_pilot_tag_index_info gauge\n");
        }
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_tag_index_info"
                         "{zone=\"%V\",backend=\"%s\"} 1\n",
                         &s->name,
                         ngx_http_cache_pilot_backend_str(s->tag_backend));
    }

    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        if (!s->has_tag_index) {
            continue;
        }

        if (i == 0 || !snaps[i - 1].has_tag_index) {
            p = ngx_slprintf(p, last,
                             "# HELP nginx_cache_pilot_tag_queue_size"
                             " Current number of pending tag index operations in the queue\n"
                             "# TYPE nginx_cache_pilot_tag_queue_size gauge\n");
        }
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_tag_queue_size{zone=\"%V\"} %ui\n",
                         &s->name, s->queue_size);
    }

    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        if (!s->has_tag_index) {
            continue;
        }

        if (i == 0 || !snaps[i - 1].has_tag_index) {
            p = ngx_slprintf(p, last,
                             "# HELP nginx_cache_pilot_tag_queue_capacity"
                             " Maximum capacity of the tag index operation queue\n"
                             "# TYPE nginx_cache_pilot_tag_queue_capacity gauge\n");
        }
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_tag_queue_capacity{zone=\"%V\"} %ui\n",
                         &s->name, s->queue_capacity);
    }

    for (i = 0; i < nzones; i++) {
        s = &snaps[i];
        if (!s->has_tag_index) {
            continue;
        }

        if (i == 0 || !snaps[i - 1].has_tag_index) {
            p = ngx_slprintf(p, last,
                             "# HELP nginx_cache_pilot_tag_queue_dropped_total"
                             " Tag index operations dropped due to queue overflow\n"
                             "# TYPE nginx_cache_pilot_tag_queue_dropped_total counter\n");
        }
        p = ngx_slprintf(p, last,
                         "nginx_cache_pilot_tag_queue_dropped_total{zone=\"%V\"} %ui\n",
                         &s->name, s->queue_dropped);
    }

    return p;
}


/* ── Shared-memory zone init callback ── */

ngx_int_t
ngx_http_cache_pilot_metrics_init_zone(ngx_shm_zone_t *shm_zone, void *data) {
    ngx_http_cache_pilot_metrics_shctx_t *old;
    ngx_slab_pool_t                      *shpool;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    old    = data;

    if (old != NULL) {
        /* On config reload: reuse existing counters so they survive nginx -s reload */
        shm_zone->data = old;
        return NGX_OK;
    }

    shm_zone->data = ngx_slab_alloc(shpool,
                                    sizeof(ngx_http_cache_pilot_metrics_shctx_t));
    if (shm_zone->data == NULL) {
        ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                      "cache_pilot metrics: out of shared memory");
        return NGX_ERROR;
    }

    ngx_memzero(shm_zone->data,
                sizeof(ngx_http_cache_pilot_metrics_shctx_t));

    return NGX_OK;
}


/* ── Register metrics shm zone (called from ngx_http_cache_pilot_init_main_conf) ── */

ngx_int_t
ngx_http_cache_pilot_metrics_init_conf(ngx_conf_t *cf,
                                       ngx_http_cache_pilot_main_conf_t *pmcf) {
    static ngx_str_t  zone_name = ngx_string("ngx_cache_pilot_metrics");
    ngx_shm_zone_t   *zone;

    if (pmcf->metrics_zone != NULL) {
        return NGX_OK;
    }

    zone = ngx_shared_memory_add(cf, &zone_name,
                                 4 * ngx_pagesize,
                                 &ngx_http_cache_pilot_module);
    if (zone == NULL) {
        return NGX_ERROR;
    }

    zone->init = ngx_http_cache_pilot_metrics_init_zone;
    pmcf->metrics_zone = zone;

    return NGX_OK;
}


/* ── HTTP handler ── */

ngx_int_t
ngx_http_cache_pilot_metrics_handler(ngx_http_request_t *r) {
    ngx_http_cache_pilot_main_conf_t      *pmcf;
    ngx_http_cache_pilot_loc_conf_t       *cplcf;
    ngx_http_cache_pilot_stat_zone_t      *stat_zones;
    ngx_http_cache_pilot_zone_snapshot_t  *snaps;
    ngx_uint_t                             nzones, fmt, i;
    u_char                                *buf, *p;
    size_t                                 buf_size;
    ngx_buf_t                             *b;
    ngx_chain_t                            out;
    ngx_table_elt_t                       *h;
    ngx_int_t                              rc;
    ngx_str_t                              ct;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    pmcf  = ngx_http_get_module_main_conf(r, ngx_http_cache_pilot_module);
    cplcf = ngx_http_get_module_loc_conf(r, ngx_http_cache_pilot_module);

    if (cplcf->stat_zones == NULL || cplcf->stat_zones->nelts == 0) {
        return NGX_HTTP_NO_CONTENT;
    }

    stat_zones = cplcf->stat_zones->elts;
    nzones     = cplcf->stat_zones->nelts;

    snaps = ngx_palloc(r->pool,
                       nzones * sizeof(ngx_http_cache_pilot_zone_snapshot_t));
    if (snaps == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    for (i = 0; i < nzones; i++) {
        ngx_http_cache_pilot_snapshot_zone(&stat_zones[i], pmcf, &snaps[i]);
    }

    fmt = ngx_http_cache_pilot_negotiate_format(r);

    /*
     * Pre-allocate a buffer large enough for the response.
     * Generous upper bounds: 2 KB base + ~1.5 KB per zone for Prometheus,
     * 512 B base + ~600 B per zone for JSON.
     */
    buf_size = (fmt == NGX_CACHE_PILOT_METRICS_FORMAT_PROMETHEUS)
               ? (2048 + nzones * 1536)
               : (512  + nzones * 600);
    buf_size = ngx_align(buf_size, ngx_pagesize);

    buf = ngx_palloc(r->pool, buf_size);
    if (buf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (fmt == NGX_CACHE_PILOT_METRICS_FORMAT_PROMETHEUS) {
        p = ngx_http_cache_pilot_write_prometheus(
                buf, buf + buf_size, nzones, snaps, pmcf->metrics);
        ngx_str_set(&ct, "text/plain; version=0.0.4; charset=utf-8");
    } else {
        p = ngx_http_cache_pilot_write_json(
                buf, buf + buf_size, nzones, snaps, pmcf->metrics);
        ngx_str_set(&ct, "application/json");
    }

    /* Build response headers */
    r->headers_out.status           = NGX_HTTP_OK;
    r->headers_out.content_length_n = p - buf;
    r->headers_out.content_type     = ct;
    r->headers_out.content_type_len = ct.len;
    r->headers_out.content_type_lowcase = NULL;

    h = ngx_list_push(&r->headers_out.headers);
    if (h != NULL) {
        h->hash = 1;
        ngx_str_set(&h->key,   "Cache-Control");
        ngx_str_set(&h->value, "no-store");
        h->lowcase_key = (u_char *) "cache-control";
    }

    if (r->method == NGX_HTTP_HEAD) {
        return ngx_http_send_header(r);
    }

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b = ngx_calloc_buf(r->pool);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->pos          = buf;
    b->last         = p;
    b->memory       = 1;
    b->last_buf     = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    out.buf  = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}

#endif /* NGX_HTTP_CACHE */
