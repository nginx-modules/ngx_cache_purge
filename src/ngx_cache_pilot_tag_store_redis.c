#include "ngx_cache_pilot_tag_store_internal.h"

#if (NGX_LINUX)

static void ngx_http_cache_tag_store_redis_close(
    ngx_http_cache_tag_store_t *store);
static ngx_int_t ngx_http_cache_tag_store_redis_begin_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_commit_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_rollback_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_upsert_file_meta(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    ngx_str_t *cache_key_text, time_t mtime, off_t size, ngx_array_t *tags,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_collect_paths_by_exact_key(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *key_text, ngx_array_t **paths, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_collect_paths_by_key_prefix(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *prefix, ngx_array_t **paths, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_delete_file(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_collect_paths_by_tags(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_array_t *tags, ngx_array_t **paths, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_get_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_set_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
static void ngx_http_cache_tag_store_redis_close_socket(
    ngx_http_cache_tag_store_t *store);
static ngx_int_t ngx_http_cache_tag_store_redis_ensure_connected(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_connect(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_send_command(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log, ngx_uint_t argc,
    ngx_str_t *argv);
static ngx_int_t ngx_http_cache_tag_store_redis_discard_reply(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_read_string_array(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_array_t **values,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_read_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_http_cache_tag_zone_state_t *state,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_read_byte(
    ngx_http_cache_tag_store_t *store, u_char *byte, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_read_line(
    ngx_http_cache_tag_store_t *store, u_char *buf, size_t size, size_t *len,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_read_exact(
    ngx_http_cache_tag_store_t *store, u_char *buf, size_t len, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_send_all(
    ngx_http_cache_tag_store_t *store, u_char *buf, size_t len, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_make_key(ngx_pool_t *pool,
        ngx_str_t *prefix, ngx_str_t *part1, ngx_str_t *part2, ngx_str_t *out);
static ngx_int_t ngx_http_cache_tag_store_redis_delete_keys(
    ngx_http_cache_tag_store_t *store, ngx_str_t *first, ngx_str_t *second,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_do_upsert_file_meta(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    ngx_str_t *cache_key_text, time_t mtime, off_t size, ngx_array_t *tags,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_do_collect_paths_by_exact_key(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *key_text, ngx_array_t **paths, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_do_collect_paths_by_key_prefix(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *prefix, ngx_array_t **paths, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_push_unique_path(
    ngx_pool_t *pool, ngx_array_t *result, ngx_str_t *candidate);
static ngx_int_t ngx_http_cache_tag_store_redis_read_bulk_string(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *out,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_do_get_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_do_set_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_do_delete_file(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_redis_do_collect_paths_by_tags(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_array_t *tags, ngx_array_t **paths, ngx_log_t *log);

static ngx_str_t ngx_http_cache_tag_redis_zone_prefix =
    ngx_string("cache_tag:zone:");
static ngx_str_t ngx_http_cache_tag_redis_tag_prefix =
    ngx_string("cache_tag:tag:");
static ngx_str_t ngx_http_cache_tag_redis_file_prefix =
    ngx_string("cache_tag:file:");
static ngx_str_t ngx_http_cache_tag_redis_filemeta_prefix =
    ngx_string("cache_tag:filemeta:");
static ngx_str_t ngx_http_cache_tag_redis_keyidx_prefix =
    ngx_string("cache_key:keyidx:");
static ngx_str_t ngx_http_cache_tag_redis_pfxidx_prefix =
    ngx_string("cache_key:pfxidx:");
static ngx_str_t ngx_http_cache_tag_redis_cmd_auth = ngx_string("AUTH");
static ngx_str_t ngx_http_cache_tag_redis_cmd_del = ngx_string("DEL");
static ngx_str_t ngx_http_cache_tag_redis_cmd_hmget = ngx_string("HMGET");
static ngx_str_t ngx_http_cache_tag_redis_cmd_hset = ngx_string("HSET");
static ngx_str_t ngx_http_cache_tag_redis_cmd_sadd = ngx_string("SADD");
static ngx_str_t ngx_http_cache_tag_redis_cmd_select = ngx_string("SELECT");
static ngx_str_t ngx_http_cache_tag_redis_cmd_smembers = ngx_string("SMEMBERS");
static ngx_str_t ngx_http_cache_tag_redis_cmd_srem = ngx_string("SREM");
static ngx_str_t ngx_http_cache_tag_redis_cmd_sunion = ngx_string("SUNION");
static ngx_str_t ngx_http_cache_tag_redis_field_bootstrap =
    ngx_string("bootstrap_complete");
static ngx_str_t ngx_http_cache_tag_redis_field_last_bootstrap =
    ngx_string("last_bootstrap_at");
static ngx_str_t ngx_http_cache_tag_redis_field_mtime = ngx_string("mtime");
static ngx_str_t ngx_http_cache_tag_redis_field_size = ngx_string("size");
static ngx_str_t ngx_http_cache_tag_redis_field_cache_key = ngx_string("cache_key");
static ngx_str_t ngx_http_cache_tag_redis_cmd_zadd = ngx_string("ZADD");
static ngx_str_t ngx_http_cache_tag_redis_cmd_zrem = ngx_string("ZREM");
static ngx_str_t ngx_http_cache_tag_redis_cmd_zrangebylex =
    ngx_string("ZRANGEBYLEX");
static ngx_str_t ngx_http_cache_tag_redis_cmd_hget = ngx_string("HGET");
static ngx_str_t ngx_http_cache_tag_redis_cmd_scard = ngx_string("SCARD");
static ngx_str_t ngx_http_cache_tag_redis_score_zero = ngx_string("0");

static const ngx_http_cache_tag_store_ops_t ngx_http_cache_tag_store_redis_ops = {
    ngx_http_cache_tag_store_redis_close,
    ngx_http_cache_tag_store_redis_begin_batch,
    ngx_http_cache_tag_store_redis_commit_batch,
    ngx_http_cache_tag_store_redis_rollback_batch,
    ngx_http_cache_tag_store_redis_upsert_file_meta,
    ngx_http_cache_tag_store_redis_delete_file,
    ngx_http_cache_tag_store_redis_collect_paths_by_tags,
    ngx_http_cache_tag_store_redis_collect_paths_by_exact_key,
    ngx_http_cache_tag_store_redis_collect_paths_by_key_prefix,
    ngx_http_cache_tag_store_redis_get_zone_state,
    ngx_http_cache_tag_store_redis_set_zone_state
};

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_redis_open(ngx_http_cache_pilot_main_conf_t *pmcf,
                                    ngx_flag_t readonly, ngx_log_t *log) {
    ngx_http_cache_tag_store_t  *store;

    if (pmcf == NULL || pmcf->backend != NGX_HTTP_CACHE_TAG_BACKEND_REDIS) {
        return NULL;
    }

    store = ngx_pcalloc(ngx_cycle->pool, sizeof(ngx_http_cache_tag_store_t));
    if (store == NULL) {
        return NULL;
    }

    store->ops = &ngx_http_cache_tag_store_redis_ops;
    store->backend = NGX_HTTP_CACHE_TAG_BACKEND_REDIS;
    store->readonly = readonly;
    store->u.redis.pmcf = pmcf;
    store->u.redis.conn = NULL;
    store->u.redis.fd = (ngx_socket_t) NGX_INVALID_FILE;

    if (ngx_http_cache_tag_store_redis_connect(store, log) != NGX_OK) {
        ngx_http_cache_tag_store_close(store);
        return NULL;
    }

    return store;
}

static void
ngx_http_cache_tag_store_redis_close(ngx_http_cache_tag_store_t *store) {
    ngx_http_cache_tag_store_redis_close_socket(store);
}

static ngx_int_t
ngx_http_cache_tag_store_redis_begin_batch(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    (void) store;
    (void) log;
    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_commit_batch(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    (void) store;
    (void) log;
    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_rollback_batch(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    (void) store;
    (void) log;
    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_upsert_file_meta(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, ngx_str_t *cache_key_text,
        time_t mtime, off_t size, ngx_array_t *tags, ngx_log_t *log) {
    ngx_uint_t  retry;
    ngx_int_t   rc;

    rc = NGX_ERROR;
    for (retry = 0; retry < 2; retry++) {
        if (retry > 0) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag redis upsert_file_meta failed, "
                          "retrying after reconnect for zone \"%V\" path \"%V\"",
                          zone_name, path);
        }
        if (ngx_http_cache_tag_store_redis_ensure_connected(store, log)
                != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_http_cache_tag_store_redis_do_upsert_file_meta(
                 store, zone_name, path, cache_key_text, mtime, size, tags, log);
        if (rc == NGX_OK) {
            break;
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_do_upsert_file_meta(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_str_t *path, ngx_str_t *cache_key_text,
    time_t mtime, off_t size, ngx_array_t *tags,
    ngx_log_t *log) {
    ngx_pool_t   *pool;
    ngx_array_t  *old_tags;
    ngx_str_t     old_key_text;
    ngx_str_t    *tag;
    ngx_str_t     file_key, filemeta_key, tag_key, keyidx_key, pfxidx_key;
    ngx_str_t     meta_args[8];
    ngx_int_t     scard;
    ngx_uint_t    meta_argc;
    ngx_uint_t    i;
    u_char        mtime_buf[NGX_TIME_T_LEN + 1];
    u_char        scard_line[64];
    u_char        size_buf[NGX_OFF_T_LEN + 1];
    ngx_array_t  *add_cmd;
    ngx_str_t    *arg;
    ngx_str_t     args[5];
    size_t        scard_len;

    pool = ngx_create_pool(8192, log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_redis_make_key(pool,
            &ngx_http_cache_tag_redis_file_prefix, zone_name, path, &file_key)
            != NGX_OK
            || ngx_http_cache_tag_store_redis_make_key(pool,
                    &ngx_http_cache_tag_redis_filemeta_prefix, zone_name, path,
                    &filemeta_key) != NGX_OK
            || ngx_http_cache_tag_store_redis_make_key(pool,
                    &ngx_http_cache_tag_redis_pfxidx_prefix, zone_name, NULL,
                    &pfxidx_key) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    /* Read old tags */
    old_tags = NULL;
    args[0] = ngx_http_cache_tag_redis_cmd_smembers;
    args[1] = file_key;
    if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, args)
            != NGX_OK
            || ngx_http_cache_tag_store_redis_read_string_array(store, pool,
                    &old_tags, log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    /* Read old cache key text from filemeta hash */
    ngx_str_null(&old_key_text);
    args[0] = ngx_http_cache_tag_redis_cmd_hget;
    args[1] = filemeta_key;
    args[2] = ngx_http_cache_tag_redis_field_cache_key;
    if (ngx_http_cache_tag_store_redis_send_command(store, log, 3, args)
            != NGX_OK
            || ngx_http_cache_tag_store_redis_read_bulk_string(store, pool,
                    &old_key_text, log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    /* Pipeline: SREM old tag→path mappings */
    if (old_tags != NULL) {
        tag = old_tags->elts;
        for (i = 0; i < old_tags->nelts; i++) {
            if (ngx_http_cache_tag_store_redis_make_key(pool,
                    &ngx_http_cache_tag_redis_tag_prefix, zone_name, &tag[i],
                    &tag_key) != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }

            args[0] = ngx_http_cache_tag_redis_cmd_srem;
            args[1] = tag_key;
            args[2] = *path;
            if (ngx_http_cache_tag_store_redis_send_command(store, log, 3,
                    args) != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }

        for (i = 0; i < old_tags->nelts; i++) {
            if (ngx_http_cache_tag_store_redis_discard_reply(store, log)
                    != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }
    }

    /* Remove path from old key index entry, clean pfxidx if set empties */
    if (old_key_text.len > 0) {
        if (ngx_http_cache_tag_store_redis_make_key(pool,
                &ngx_http_cache_tag_redis_keyidx_prefix, zone_name,
                &old_key_text, &keyidx_key) != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        args[0] = ngx_http_cache_tag_redis_cmd_srem;
        args[1] = keyidx_key;
        args[2] = *path;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 3, args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        args[0] = ngx_http_cache_tag_redis_cmd_scard;
        args[1] = keyidx_key;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_read_line(store, scard_line,
                        sizeof(scard_line), &scard_len, log) != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        scard = (scard_line[0] == ':')
                ? ngx_atoi(scard_line + 1, scard_len - 1)
                : -1;

        if (scard == 0) {
            /* keyidx set is empty — remove key text from pfxidx */
            args[0] = ngx_http_cache_tag_redis_cmd_zrem;
            args[1] = pfxidx_key;
            args[2] = old_key_text;
            if (ngx_http_cache_tag_store_redis_send_command(store, log, 3,
                    args) != NGX_OK
                    || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                    != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }
    }

    if (ngx_http_cache_tag_store_redis_delete_keys(store, &file_key,
            &filemeta_key, log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    if ((tags == NULL || tags->nelts == 0)
            && (cache_key_text == NULL || cache_key_text->len == 0)) {
        ngx_destroy_pool(pool);
        return NGX_OK;
    }

    /* Write new tags to file_key set */
    if (tags != NULL && tags->nelts > 0) {
        tag = tags->elts;
        add_cmd = ngx_array_create(pool, tags->nelts + 2, sizeof(ngx_str_t));
        if (add_cmd == NULL) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }
        arg = ngx_array_push(add_cmd);
        if (arg == NULL) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }
        *arg = ngx_http_cache_tag_redis_cmd_sadd;
        arg = ngx_array_push(add_cmd);
        if (arg == NULL) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }
        *arg = file_key;
        for (i = 0; i < tags->nelts; i++) {
            arg = ngx_array_push(add_cmd);
            if (arg == NULL) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
            *arg = tag[i];
        }
        if (ngx_http_cache_tag_store_redis_send_command(store, log,
                add_cmd->nelts, add_cmd->elts) != NGX_OK
                || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }
    }

    /* Write filemeta hash: mtime, size, and optionally cache_key */
    meta_argc = 6;
    meta_args[0] = ngx_http_cache_tag_redis_cmd_hset;
    meta_args[1] = filemeta_key;
    meta_args[2] = ngx_http_cache_tag_redis_field_mtime;
    meta_args[3].data = mtime_buf;
    meta_args[3].len = ngx_sprintf(mtime_buf, "%T", mtime) - mtime_buf;
    meta_args[4] = ngx_http_cache_tag_redis_field_size;
    meta_args[5].data = size_buf;
    meta_args[5].len = ngx_sprintf(size_buf, "%O", size) - size_buf;

    if (cache_key_text != NULL && cache_key_text->len > 0) {
        meta_args[6] = ngx_http_cache_tag_redis_field_cache_key;
        meta_args[7] = *cache_key_text;
        meta_argc = 8;
    }

    if (ngx_http_cache_tag_store_redis_send_command(store, log, meta_argc,
            meta_args) != NGX_OK
            || ngx_http_cache_tag_store_redis_discard_reply(store, log)
            != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    /* Write per-tag SADD commands (pipeline) */
    if (tags != NULL) {
        tag = tags->elts;
        for (i = 0; i < tags->nelts; i++) {
            if (ngx_http_cache_tag_store_redis_make_key(pool,
                    &ngx_http_cache_tag_redis_tag_prefix, zone_name, &tag[i],
                    &tag_key) != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }

            args[0] = ngx_http_cache_tag_redis_cmd_sadd;
            args[1] = tag_key;
            args[2] = *path;
            if (ngx_http_cache_tag_store_redis_send_command(store, log, 3,
                    args) != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }

        for (i = 0; i < tags->nelts; i++) {
            if (ngx_http_cache_tag_store_redis_discard_reply(store, log)
                    != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }
    }

    /* Write key index: SADD keyidx path and ZADD pfxidx 0 key_text */
    if (cache_key_text != NULL && cache_key_text->len > 0) {
        if (ngx_http_cache_tag_store_redis_make_key(pool,
                &ngx_http_cache_tag_redis_keyidx_prefix, zone_name,
                cache_key_text, &keyidx_key) != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        args[0] = ngx_http_cache_tag_redis_cmd_sadd;
        args[1] = keyidx_key;
        args[2] = *path;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 3, args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        args[0] = ngx_http_cache_tag_redis_cmd_zadd;
        args[1] = pfxidx_key;
        args[2] = ngx_http_cache_tag_redis_score_zero;
        args[3] = *cache_key_text;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 4, args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }
    }

    ngx_destroy_pool(pool);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_delete_file(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path,
        ngx_log_t *log) {
    ngx_uint_t  retry;
    ngx_int_t   rc;

    rc = NGX_ERROR;
    for (retry = 0; retry < 2; retry++) {
        if (retry > 0) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag redis delete_file failed, "
                          "retrying after reconnect for zone \"%V\" path \"%V\"",
                          zone_name, path);
        }
        if (ngx_http_cache_tag_store_redis_ensure_connected(store, log)
                != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_http_cache_tag_store_redis_do_delete_file(
                 store, zone_name, path, log);
        if (rc == NGX_OK) {
            break;
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_do_delete_file(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_str_t *path, ngx_log_t *log) {
    ngx_pool_t   *pool;
    ngx_array_t  *old_tags;
    ngx_str_t     old_key_text;
    ngx_str_t    *tag;
    ngx_str_t     file_key, filemeta_key, tag_key, keyidx_key, pfxidx_key;
    ngx_str_t     args[3];
    ngx_uint_t    i;

    pool = ngx_create_pool(4096, log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_redis_make_key(pool,
            &ngx_http_cache_tag_redis_file_prefix, zone_name, path, &file_key)
            != NGX_OK
            || ngx_http_cache_tag_store_redis_make_key(pool,
                    &ngx_http_cache_tag_redis_filemeta_prefix, zone_name, path,
                    &filemeta_key) != NGX_OK
            || ngx_http_cache_tag_store_redis_make_key(pool,
                    &ngx_http_cache_tag_redis_pfxidx_prefix, zone_name, NULL,
                    &pfxidx_key) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    args[0] = ngx_http_cache_tag_redis_cmd_smembers;
    args[1] = file_key;
    if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, args) != NGX_OK
            || ngx_http_cache_tag_store_redis_read_string_array(store, pool,
                    &old_tags, log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    /* Read old cache key text */
    ngx_str_null(&old_key_text);
    args[0] = ngx_http_cache_tag_redis_cmd_hget;
    args[1] = filemeta_key;
    args[2] = ngx_http_cache_tag_redis_field_cache_key;
    if (ngx_http_cache_tag_store_redis_send_command(store, log, 3, args)
            != NGX_OK
            || ngx_http_cache_tag_store_redis_read_bulk_string(store, pool,
                    &old_key_text, log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    if (old_tags != NULL) {
        tag = old_tags->elts;
        for (i = 0; i < old_tags->nelts; i++) {
            if (ngx_http_cache_tag_store_redis_make_key(pool,
                    &ngx_http_cache_tag_redis_tag_prefix, zone_name, &tag[i],
                    &tag_key) != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }

            args[0] = ngx_http_cache_tag_redis_cmd_srem;
            args[1] = tag_key;
            args[2] = *path;
            if (ngx_http_cache_tag_store_redis_send_command(store, log, 3, args)
                    != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }

        for (i = 0; i < old_tags->nelts; i++) {
            if (ngx_http_cache_tag_store_redis_discard_reply(store, log)
                    != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }
    }

    /* Remove from key index; clean pfxidx if the keyidx set becomes empty */
    if (old_key_text.len > 0) {
        ngx_int_t  scard;
        u_char     scard_line[64];
        size_t     scard_len;

        if (ngx_http_cache_tag_store_redis_make_key(pool,
                &ngx_http_cache_tag_redis_keyidx_prefix, zone_name,
                &old_key_text, &keyidx_key) != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        args[0] = ngx_http_cache_tag_redis_cmd_srem;
        args[1] = keyidx_key;
        args[2] = *path;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 3, args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        args[0] = ngx_http_cache_tag_redis_cmd_scard;
        args[1] = keyidx_key;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_read_line(store, scard_line,
                        sizeof(scard_line), &scard_len, log) != NGX_OK) {
            ngx_destroy_pool(pool);
            return NGX_ERROR;
        }

        scard = (scard_line[0] == ':')
                ? ngx_atoi(scard_line + 1, scard_len - 1)
                : -1;

        if (scard == 0) {
            args[0] = ngx_http_cache_tag_redis_cmd_zrem;
            args[1] = pfxidx_key;
            args[2] = old_key_text;
            if (ngx_http_cache_tag_store_redis_send_command(store, log, 3,
                    args) != NGX_OK
                    || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                    != NGX_OK) {
                ngx_destroy_pool(pool);
                return NGX_ERROR;
            }
        }
    }

    if (ngx_http_cache_tag_store_redis_delete_keys(store, &file_key, &filemeta_key,
            log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    ngx_destroy_pool(pool);

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_collect_paths_by_tags(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_array_t *tags, ngx_array_t **paths, ngx_log_t *log) {
    ngx_uint_t  retry;
    ngx_int_t   rc;

    rc = NGX_ERROR;
    for (retry = 0; retry < 2; retry++) {
        if (retry > 0) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag redis collect_paths failed, "
                          "retrying after reconnect for zone \"%V\"", zone_name);
        }
        if (ngx_http_cache_tag_store_redis_ensure_connected(store, log)
                != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_http_cache_tag_store_redis_do_collect_paths_by_tags(
                 store, pool, zone_name, tags, paths, log);
        if (rc == NGX_OK) {
            break;
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_do_collect_paths_by_tags(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_array_t *tags, ngx_array_t **paths, ngx_log_t *log) {
    ngx_array_t  *args, *result;
    ngx_str_t    *tag, *arg;
    ngx_uint_t    i;

    result = ngx_array_create(pool, 8, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    if (tags == NULL || tags->nelts == 0) {
        *paths = result;
        return NGX_OK;
    }

    args = ngx_array_create(pool, tags->nelts + 1, sizeof(ngx_str_t));
    if (args == NULL) {
        return NGX_ERROR;
    }

    arg = ngx_array_push(args);
    if (arg == NULL) {
        return NGX_ERROR;
    }
    *arg = ngx_http_cache_tag_redis_cmd_sunion;

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        arg = ngx_array_push(args);
        if (arg == NULL) {
            return NGX_ERROR;
        }
        if (ngx_http_cache_tag_store_redis_make_key(pool,
                &ngx_http_cache_tag_redis_tag_prefix, zone_name, &tag[i], arg)
                != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (ngx_http_cache_tag_store_redis_send_command(store, log, args->nelts,
            args->elts) != NGX_OK
            || ngx_http_cache_tag_store_redis_read_string_array(store, pool,
                    &result, log) != NGX_OK) {
        return NGX_ERROR;
    }

    *paths = result;
    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_get_zone_state(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log) {
    ngx_uint_t  retry;
    ngx_int_t   rc;

    rc = NGX_ERROR;
    for (retry = 0; retry < 2; retry++) {
        if (retry > 0) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag redis get_zone_state failed, "
                          "retrying after reconnect for zone \"%V\"", zone_name);
        }
        if (ngx_http_cache_tag_store_redis_ensure_connected(store, log) != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_http_cache_tag_store_redis_do_get_zone_state(store, zone_name,
                state, log);
        if (rc == NGX_OK) {
            break;
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_do_get_zone_state(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log) {
    u_char      zone_key_buf[1024];
    ngx_str_t   zone_key;
    ngx_str_t   args[4];
    size_t      key_len;

    state->bootstrap_complete = 0;
    state->last_bootstrap_at = 0;

    key_len = ngx_http_cache_tag_redis_zone_prefix.len + zone_name->len;
    if (key_len >= sizeof(zone_key_buf)) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "cache_tag redis zone key too long for zone \"%V\"",
                      zone_name);
        return NGX_ERROR;
    }

    zone_key.data = zone_key_buf;
    zone_key.len = key_len;
    ngx_memcpy(zone_key_buf,
               ngx_http_cache_tag_redis_zone_prefix.data,
               ngx_http_cache_tag_redis_zone_prefix.len);
    ngx_memcpy(zone_key_buf + ngx_http_cache_tag_redis_zone_prefix.len,
               zone_name->data, zone_name->len);
    zone_key_buf[key_len] = '\0';

    args[0] = ngx_http_cache_tag_redis_cmd_hmget;
    args[1] = zone_key;
    args[2] = ngx_http_cache_tag_redis_field_bootstrap;
    args[3] = ngx_http_cache_tag_redis_field_last_bootstrap;

    if (ngx_http_cache_tag_store_redis_send_command(store, log, 4, args) != NGX_OK) {
        return NGX_ERROR;
    }

    return ngx_http_cache_tag_store_redis_read_zone_state(store, state, log);
}

static ngx_int_t
ngx_http_cache_tag_store_redis_set_zone_state(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log) {
    ngx_uint_t  retry;
    ngx_int_t   rc;

    rc = NGX_ERROR;
    for (retry = 0; retry < 2; retry++) {
        if (retry > 0) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag redis set_zone_state failed, "
                          "retrying after reconnect for zone \"%V\"", zone_name);
        }
        if (ngx_http_cache_tag_store_redis_ensure_connected(store, log) != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_http_cache_tag_store_redis_do_set_zone_state(store, zone_name,
                state, log);
        if (rc == NGX_OK) {
            break;
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_do_set_zone_state(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log) {
    u_char     zone_key_buf[1024];
    ngx_str_t  zone_key;
    ngx_str_t  args[6];
    u_char     bootstrap_buf[2];
    u_char     ts_buf[NGX_TIME_T_LEN + 1];
    size_t     key_len;

    key_len = ngx_http_cache_tag_redis_zone_prefix.len + zone_name->len;
    if (key_len >= sizeof(zone_key_buf)) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "cache_tag redis zone key too long for zone \"%V\"",
                      zone_name);
        return NGX_ERROR;
    }

    zone_key.data = zone_key_buf;
    zone_key.len = key_len;
    ngx_memcpy(zone_key_buf,
               ngx_http_cache_tag_redis_zone_prefix.data,
               ngx_http_cache_tag_redis_zone_prefix.len);
    ngx_memcpy(zone_key_buf + ngx_http_cache_tag_redis_zone_prefix.len,
               zone_name->data, zone_name->len);
    zone_key_buf[key_len] = '\0';

    bootstrap_buf[0] = state->bootstrap_complete ? '1' : '0';
    args[0] = ngx_http_cache_tag_redis_cmd_hset;
    args[1] = zone_key;
    args[2] = ngx_http_cache_tag_redis_field_bootstrap;
    args[3].data = bootstrap_buf;
    args[3].len = 1;
    args[4] = ngx_http_cache_tag_redis_field_last_bootstrap;
    args[5].data = ts_buf;
    args[5].len = ngx_sprintf(ts_buf, "%T", state->last_bootstrap_at) - ts_buf;

    if (ngx_http_cache_tag_store_redis_send_command(store, log, 6, args) != NGX_OK) {
        return NGX_ERROR;
    }

    return ngx_http_cache_tag_store_redis_discard_reply(store, log);
}

static void
ngx_http_cache_tag_store_redis_close_socket(ngx_http_cache_tag_store_t *store) {
    if (store->u.redis.conn != NULL) {
        ngx_close_connection(store->u.redis.conn);
        store->u.redis.conn = NULL;
        store->u.redis.fd = (ngx_socket_t) NGX_INVALID_FILE;
    }
    store->u.redis.recv_pos = 0;
    store->u.redis.recv_len = 0;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_ensure_connected(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    if (store->u.redis.conn != NULL) {
        return NGX_OK;
    }

    return ngx_http_cache_tag_store_redis_connect(store, log);
}

static ngx_int_t
ngx_http_cache_tag_store_redis_connect(ngx_http_cache_tag_store_t *store,
                                       ngx_log_t *log) {
    ngx_http_cache_tag_redis_conf_t *conf;
    ngx_connection_t                *conn;
    ngx_str_t                        auth_args[2];
    ngx_str_t                        select_args[2];
    u_char                           db_buf[NGX_INT_T_LEN + 1];
    int                              fd;

    conf = &store->u.redis.pmcf->redis;
    fd = NGX_INVALID_FILE;

    if (conf->use_unix) {
        struct sockaddr_un addr;

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == -1) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "redis socket(AF_UNIX) failed");
            return NGX_ERROR;
        }

        ngx_memzero(&addr, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (conf->unix_path.len >= sizeof(addr.sun_path)) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "redis unix socket path too long: \"%V\"",
                          &conf->unix_path);
            close(fd);
            return NGX_ERROR;
        }

        ngx_memcpy(addr.sun_path, conf->unix_path.data, conf->unix_path.len);
        addr.sun_path[conf->unix_path.len] = '\0';

        if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "redis connect failed for unix socket \"%V\"",
                          &conf->unix_path);
            close(fd);
            return NGX_ERROR;
        }

    } else {
        /* Use the address resolved once at config-parse time so that this
         * reconnect path (called from timer callbacks and request handlers)
         * never blocks the event loop with a getaddrinfo() call. */
        if (!conf->resolved) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "redis: no pre-resolved address for \"%V\"",
                          &conf->endpoint);
            return NGX_ERROR;
        }

        fd = socket(conf->resolved_addr.ss_family, SOCK_STREAM, 0);
        if (fd == -1) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "redis socket() failed");
            return NGX_ERROR;
        }

        if (connect(fd, (struct sockaddr *) &conf->resolved_addr,
                    conf->resolved_addrlen) == -1) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "redis connect failed for \"%V\"", &conf->endpoint);
            close(fd);
            return NGX_ERROR;
        }
    }

    /* Register the fd with the nginx cycle so the connection table owns it.
     * This ensures the fd is closed on worker reload/exit and is visible to
     * the event backend. */
    conn = ngx_get_connection(fd, log);
    if (conn == NULL) {
        close(fd);
        return NGX_ERROR;
    }
    conn->log = log;

    store->u.redis.conn = conn;
    store->u.redis.fd = fd;

    if (conf->password.len > 0) {
        auth_args[0] = ngx_http_cache_tag_redis_cmd_auth;
        auth_args[1] = conf->password;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, auth_args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                != NGX_OK) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            return NGX_ERROR;
        }
    }

    if (conf->db > 0) {
        select_args[0] = ngx_http_cache_tag_redis_cmd_select;
        select_args[1].data = db_buf;
        select_args[1].len = ngx_sprintf(db_buf, "%ui", conf->db) - db_buf;
        if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, select_args)
                != NGX_OK
                || ngx_http_cache_tag_store_redis_discard_reply(store, log)
                != NGX_OK) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_send_command(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log, ngx_uint_t argc,
        ngx_str_t *argv) {
    u_char     header[64];
    u_char     len_buf[64];
    size_t     n;
    ngx_uint_t i;

    n = ngx_snprintf(header, sizeof(header), "*%ui\r\n", argc) - header;
    if (ngx_http_cache_tag_store_redis_send_all(store, header, n, log) != NGX_OK) {
        return NGX_ERROR;
    }

    for (i = 0; i < argc; i++) {
        n = ngx_snprintf(len_buf, sizeof(len_buf), "$%uz\r\n", argv[i].len) - len_buf;
        if (ngx_http_cache_tag_store_redis_send_all(store, len_buf, n, log) != NGX_OK
                || ngx_http_cache_tag_store_redis_send_all(store, argv[i].data,
                        argv[i].len, log) != NGX_OK
                || ngx_http_cache_tag_store_redis_send_all(store,
                        (u_char *) "\r\n", 2, log) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_discard_reply(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    u_char    line[128];
    size_t    len;
    ngx_int_t count, bulk_len, i;
    u_char    marker;

    if (ngx_http_cache_tag_store_redis_read_line(store, line, sizeof(line), &len, log)
            != NGX_OK) {
        return NGX_ERROR;
    }

    marker = line[0];
    switch (marker) {
    case '+':
    case ':':
        return NGX_OK;
    case '-':
        ngx_log_error(NGX_LOG_ERR, log, 0, "redis command failed: %*s",
                      (int)(len - 1), line + 1);
        return NGX_ERROR;
    case '$':
        bulk_len = ngx_atoi(line + 1, len - 1);
        if (bulk_len == NGX_ERROR || bulk_len < -1) {
            return NGX_ERROR;
        }
        if (bulk_len == -1) {
            return NGX_OK;
        }
        /* drain bulk string + CRLF through read_byte to keep recv buffer in sync */
        for (i = 0; i < (ngx_int_t) bulk_len + 2; i++) {
            if (ngx_http_cache_tag_store_redis_read_byte(store,
                    (u_char *) &marker, log) != NGX_OK) {
                return NGX_ERROR;
            }
        }
        return NGX_OK;
    case '*':
        count = ngx_atoi(line + 1, len - 1);
        if (count == NGX_ERROR || count < -1) {
            return NGX_ERROR;
        }
        if (count == -1) {
            return NGX_OK;
        }
        for (i = 0; i < count; i++) {
            if (ngx_http_cache_tag_store_redis_discard_reply(store, log) != NGX_OK) {
                return NGX_ERROR;
            }
        }
        return NGX_OK;
    default:
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "unexpected redis reply marker: %c", marker);
        return NGX_ERROR;
    }
}

static ngx_int_t
ngx_http_cache_tag_store_redis_read_string_array(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_array_t **values,
    ngx_log_t *log) {
    u_char      line[128];
    size_t      len;
    ngx_int_t   count, bulk_len;
    ngx_int_t   i;
    ngx_array_t *result;
    ngx_str_t   *item;

    if (ngx_http_cache_tag_store_redis_read_line(store, line, sizeof(line), &len, log)
            != NGX_OK) {
        return NGX_ERROR;
    }

    if (line[0] == '-') {
        ngx_log_error(NGX_LOG_ERR, log, 0, "redis command failed: %*s",
                      (int)(len - 1), line + 1);
        return NGX_ERROR;
    }

    if (line[0] != '*') {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "unexpected redis array reply: %*s", (int) len, line);
        return NGX_ERROR;
    }

    count = ngx_atoi(line + 1, len - 1);
    if (count == NGX_ERROR || count < -1) {
        return NGX_ERROR;
    }

    result = ngx_array_create(pool, count > 0 ? (ngx_uint_t) count : 1,
                              sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    if (count == -1) {
        *values = result;
        return NGX_OK;
    }

    for (i = 0; i < count; i++) {
        if (ngx_http_cache_tag_store_redis_read_line(store, line, sizeof(line),
                &len, log) != NGX_OK) {
            return NGX_ERROR;
        }

        if (line[0] == '-') {
            ngx_log_error(NGX_LOG_ERR, log, 0, "redis command failed: %*s",
                          (int)(len - 1), line + 1);
            return NGX_ERROR;
        }

        if (line[0] != '$') {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "unexpected redis bulk reply: %*s", (int) len, line);
            return NGX_ERROR;
        }

        bulk_len = ngx_atoi(line + 1, len - 1);
        if (bulk_len == NGX_ERROR || bulk_len < -1) {
            return NGX_ERROR;
        }
        if (bulk_len == -1) {
            continue;
        }

        item = ngx_array_push(result);
        if (item == NULL) {
            return NGX_ERROR;
        }

        item->data = ngx_pnalloc(pool, (size_t) bulk_len + 2);
        if (item->data == NULL) {
            return NGX_ERROR;
        }

        if (ngx_http_cache_tag_store_redis_read_exact(store, item->data,
                (size_t) bulk_len + 2, log) != NGX_OK) {
            return NGX_ERROR;
        }

        item->len = (size_t) bulk_len;
        item->data[bulk_len] = '\0';
    }

    *values = result;
    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_read_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_http_cache_tag_zone_state_t *state,
    ngx_log_t *log) {
    u_char    line[128];
    size_t    len;
    ngx_int_t count, bulk_len;
    ngx_int_t i;
    u_char    value_buf[64];

    if (ngx_http_cache_tag_store_redis_read_line(store, line, sizeof(line), &len, log)
            != NGX_OK) {
        return NGX_ERROR;
    }

    if (line[0] == '-') {
        ngx_log_error(NGX_LOG_ERR, log, 0, "redis command failed: %*s",
                      (int)(len - 1), line + 1);
        return NGX_ERROR;
    }

    if (line[0] != '*') {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "unexpected redis zone-state reply: %*s", (int) len, line);
        return NGX_ERROR;
    }

    count = ngx_atoi(line + 1, len - 1);
    if (count == NGX_ERROR || count < 0) {
        return NGX_ERROR;
    }

    for (i = 0; i < count && i < 2; i++) {
        if (ngx_http_cache_tag_store_redis_read_line(store, line, sizeof(line),
                &len, log) != NGX_OK) {
            return NGX_ERROR;
        }
        if (line[0] != '$') {
            return NGX_ERROR;
        }
        bulk_len = ngx_atoi(line + 1, len - 1);
        if (bulk_len == -1) {
            continue;
        }
        if (bulk_len == NGX_ERROR || bulk_len >= (ngx_int_t) sizeof(value_buf)) {
            return NGX_ERROR;
        }
        if (ngx_http_cache_tag_store_redis_read_exact(store, value_buf,
                (size_t) bulk_len + 2, log) != NGX_OK) {
            return NGX_ERROR;
        }
        value_buf[bulk_len] = '\0';
        if (i == 0) {
            state->bootstrap_complete = ngx_atoi(value_buf, bulk_len) > 0;
        } else {
            state->last_bootstrap_at = (time_t) ngx_atoi(value_buf, bulk_len);
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_read_byte(ngx_http_cache_tag_store_t *store,
        u_char *byte, ngx_log_t *log) {
    ssize_t  n;

    if (store->u.redis.recv_pos >= store->u.redis.recv_len) {
        n = recv(store->u.redis.fd, store->u.redis.recv_buf,
                 sizeof(store->u.redis.recv_buf), 0);
        if (n == 0) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "redis connection closed");
            ngx_http_cache_tag_store_redis_close_socket(store);
            return NGX_ERROR;
        }
        if (n < 0) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "redis recv failed");
            ngx_http_cache_tag_store_redis_close_socket(store);
            return NGX_ERROR;
        }
        store->u.redis.recv_pos = 0;
        store->u.redis.recv_len = (size_t) n;
    }

    *byte = store->u.redis.recv_buf[store->u.redis.recv_pos++];
    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_read_line(ngx_http_cache_tag_store_t *store,
        u_char *buf, size_t size, size_t *len,
        ngx_log_t *log) {
    size_t  off;
    u_char  byte;

    off = 0;
    while (off + 1 < size) {
        if (ngx_http_cache_tag_store_redis_read_byte(store, &byte, log) != NGX_OK) {
            return NGX_ERROR;
        }
        buf[off] = byte;
        if (off > 0 && buf[off - 1] == '\r' && buf[off] == '\n') {
            buf[off - 1] = '\0';
            *len = off - 1;
            return NGX_OK;
        }
        off++;
    }

    ngx_log_error(NGX_LOG_ERR, log, 0, "redis reply line too long");
    ngx_http_cache_tag_store_redis_close_socket(store);
    return NGX_ERROR;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_read_exact(ngx_http_cache_tag_store_t *store,
        u_char *buf, size_t len, ngx_log_t *log) {
    size_t   have, need, off;
    ssize_t  n;

    off = 0;

    /* drain buffered bytes first */
    have = store->u.redis.recv_len - store->u.redis.recv_pos;
    if (have > 0) {
        need = (len < have) ? len : have;
        ngx_memcpy(buf, store->u.redis.recv_buf + store->u.redis.recv_pos, need);
        store->u.redis.recv_pos += need;
        off += need;
    }

    /* read remainder directly from socket */
    while (off < len) {
        n = recv(store->u.redis.fd, buf + off, len - off, 0);
        if (n <= 0) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                          "redis recv failed while reading payload");
            ngx_http_cache_tag_store_redis_close_socket(store);
            return NGX_ERROR;
        }
        off += (size_t) n;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_send_all(ngx_http_cache_tag_store_t *store,
                                        u_char *buf, size_t len, ngx_log_t *log) {
    ssize_t  n;
    size_t   off;

    off = 0;
    while (off < len) {
        n = send(store->u.redis.fd, buf + off, len - off, 0);
        if (n <= 0) {
            ngx_log_error(NGX_LOG_ERR, log, ngx_errno, "redis send failed");
            ngx_http_cache_tag_store_redis_close_socket(store);
            return NGX_ERROR;
        }
        off += (size_t) n;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_make_key(ngx_pool_t *pool, ngx_str_t *prefix,
                                        ngx_str_t *part1, ngx_str_t *part2,
                                        ngx_str_t *out) {
    size_t  len;
    u_char *p;

    len = prefix->len + part1->len;
    if (part2 != NULL) {
        len += 1 + part2->len;
    }

    out->data = ngx_pnalloc(pool, len + 1);
    if (out->data == NULL) {
        return NGX_ERROR;
    }

    p = ngx_cpymem(out->data, prefix->data, prefix->len);
    p = ngx_cpymem(p, part1->data, part1->len);
    if (part2 != NULL) {
        *p++ = ':';
        p = ngx_cpymem(p, part2->data, part2->len);
    }
    *p = '\0';
    out->len = len;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_delete_keys(ngx_http_cache_tag_store_t *store,
        ngx_str_t *first, ngx_str_t *second,
        ngx_log_t *log) {
    ngx_str_t  args[3];

    args[0] = ngx_http_cache_tag_redis_cmd_del;
    args[1] = *first;
    args[2] = *second;

    if (ngx_http_cache_tag_store_redis_send_command(store, log, 3, args) != NGX_OK) {
        return NGX_ERROR;
    }

    return ngx_http_cache_tag_store_redis_discard_reply(store, log);
}

static ngx_int_t
ngx_http_cache_tag_store_redis_read_bulk_string(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *out,
    ngx_log_t *log) {
    u_char    line[128];
    size_t    len;
    ngx_int_t bulk_len;

    ngx_str_null(out);

    if (ngx_http_cache_tag_store_redis_read_line(store, line, sizeof(line),
            &len, log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (line[0] == '-') {
        ngx_log_error(NGX_LOG_ERR, log, 0, "redis command failed: %*s",
                      (int)(len - 1), line + 1);
        return NGX_ERROR;
    }

    if (line[0] != '$') {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "unexpected redis bulk-string reply: %*s", (int) len, line);
        return NGX_ERROR;
    }

    /* Redis nil bulk string: "$-1". ngx_atoi() does not parse signs. */
    if (len == 3 && line[1] == '-' && line[2] == '1') {
        return NGX_OK;
    }

    bulk_len = ngx_atoi(line + 1, len - 1);
    if (bulk_len == NGX_ERROR || bulk_len < 0) {
        return NGX_ERROR;
    }

    out->data = ngx_pnalloc(pool, (size_t) bulk_len + 2);
    if (out->data == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_redis_read_exact(store, out->data,
            (size_t) bulk_len + 2, log) != NGX_OK) {
        return NGX_ERROR;
    }

    out->len = (size_t) bulk_len;
    out->data[bulk_len] = '\0';

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_collect_paths_by_exact_key(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *key_text, ngx_array_t **paths, ngx_log_t *log) {
    ngx_uint_t  retry;
    ngx_int_t   rc;

    rc = NGX_ERROR;
    for (retry = 0; retry < 2; retry++) {
        if (retry > 0) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag redis collect_paths_by_exact_key failed, "
                          "retrying after reconnect for zone \"%V\"", zone_name);
        }
        if (ngx_http_cache_tag_store_redis_ensure_connected(store, log) != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_http_cache_tag_store_redis_do_collect_paths_by_exact_key(
                 store, pool, zone_name, key_text, paths, log);
        if (rc == NGX_OK) {
            break;
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_do_collect_paths_by_exact_key(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *key_text, ngx_array_t **paths, ngx_log_t *log) {
    ngx_str_t  keyidx_key;
    ngx_str_t  args[2];

    if (ngx_http_cache_tag_store_redis_make_key(pool,
            &ngx_http_cache_tag_redis_keyidx_prefix, zone_name, key_text,
            &keyidx_key) != NGX_OK) {
        return NGX_ERROR;
    }

    args[0] = ngx_http_cache_tag_redis_cmd_smembers;
    args[1] = keyidx_key;

    if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, args) != NGX_OK
            || ngx_http_cache_tag_store_redis_read_string_array(store, pool,
                    paths, log) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_collect_paths_by_key_prefix(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *prefix, ngx_array_t **paths, ngx_log_t *log) {
    ngx_uint_t  retry;
    ngx_int_t   rc;

    rc = NGX_ERROR;
    for (retry = 0; retry < 2; retry++) {
        if (retry > 0) {
            ngx_http_cache_tag_store_redis_close_socket(store);
            ngx_log_error(NGX_LOG_NOTICE, log, 0,
                          "cache_tag redis collect_paths_by_key_prefix failed, "
                          "retrying after reconnect for zone \"%V\"", zone_name);
        }
        if (ngx_http_cache_tag_store_redis_ensure_connected(store, log) != NGX_OK) {
            return NGX_ERROR;
        }
        rc = ngx_http_cache_tag_store_redis_do_collect_paths_by_key_prefix(
                 store, pool, zone_name, prefix, paths, log);
        if (rc == NGX_OK) {
            break;
        }
    }

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_do_collect_paths_by_key_prefix(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_str_t *prefix, ngx_array_t **paths, ngx_log_t *log) {
    ngx_str_t    pfxidx_key, keyidx_key, range_min, range_max;
    ngx_str_t    args[4];
    ngx_array_t *key_texts, *sub_paths, *result;
    ngx_str_t   *key_text, *sub_path;
    ngx_uint_t   i, j;

    result = ngx_array_create(pool, 8, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_redis_make_key(pool,
            &ngx_http_cache_tag_redis_pfxidx_prefix, zone_name, NULL,
            &pfxidx_key) != NGX_OK) {
        return NGX_ERROR;
    }

    /* "[prefix" lower bound, "[prefix\xff" upper bound for ZRANGEBYLEX */
    range_min.len = 1 + prefix->len;
    range_min.data = ngx_pnalloc(pool, range_min.len + 1);
    if (range_min.data == NULL) {
        return NGX_ERROR;
    }
    range_min.data[0] = '[';
    ngx_memcpy(range_min.data + 1, prefix->data, prefix->len);
    range_min.data[range_min.len] = '\0';

    range_max.len = 1 + prefix->len + 1;
    range_max.data = ngx_pnalloc(pool, range_max.len + 1);
    if (range_max.data == NULL) {
        return NGX_ERROR;
    }
    range_max.data[0] = '[';
    ngx_memcpy(range_max.data + 1, prefix->data, prefix->len);
    range_max.data[1 + prefix->len] = (u_char) 0xff;
    range_max.data[range_max.len] = '\0';

    args[0] = ngx_http_cache_tag_redis_cmd_zrangebylex;
    args[1] = pfxidx_key;
    args[2] = range_min;
    args[3] = range_max;

    key_texts = NULL;
    if (ngx_http_cache_tag_store_redis_send_command(store, log, 4, args) != NGX_OK
            || ngx_http_cache_tag_store_redis_read_string_array(store, pool,
                    &key_texts, log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (key_texts == NULL || key_texts->nelts == 0) {
        *paths = result;
        return NGX_OK;
    }

    key_text = key_texts->elts;

    /* Pipeline SMEMBERS for all matched keys to reduce round trips. */
    args[0] = ngx_http_cache_tag_redis_cmd_smembers;
    for (i = 0; i < key_texts->nelts; i++) {
        if (ngx_http_cache_tag_store_redis_make_key(pool,
                &ngx_http_cache_tag_redis_keyidx_prefix, zone_name, &key_text[i],
                &keyidx_key) != NGX_OK) {
            return NGX_ERROR;
        }

        args[1] = keyidx_key;

        if (ngx_http_cache_tag_store_redis_send_command(store, log, 2, args)
                != NGX_OK) {
            return NGX_ERROR;
        }
    }

    for (i = 0; i < key_texts->nelts; i++) {
        sub_paths = NULL;
        if (ngx_http_cache_tag_store_redis_read_string_array(store, pool,
                &sub_paths, log) != NGX_OK) {
            return NGX_ERROR;
        }

        if (sub_paths == NULL || sub_paths->nelts == 0) {
            continue;
        }

        sub_path = sub_paths->elts;
        for (j = 0; j < sub_paths->nelts; j++) {
            if (ngx_http_cache_tag_store_redis_push_unique_path(pool, result,
                    &sub_path[j]) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    *paths = result;
    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_redis_push_unique_path(ngx_pool_t *pool,
        ngx_array_t *result, ngx_str_t *candidate) {
    ngx_str_t   *path;
    ngx_str_t   *out_path;
    ngx_uint_t   i;

    path = result->elts;
    for (i = 0; i < result->nelts; i++) {
        if (path[i].len == candidate->len
                && ngx_strncmp(path[i].data, candidate->data,
                               candidate->len) == 0) {
            return NGX_OK;
        }
    }

    out_path = ngx_array_push(result);
    if (out_path == NULL) {
        return NGX_ERROR;
    }

    out_path->data = ngx_pnalloc(pool, candidate->len + 1);
    if (out_path->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(out_path->data, candidate->data, candidate->len);
    out_path->len = candidate->len;
    out_path->data[candidate->len] = '\0';

    return NGX_OK;
}

#endif
