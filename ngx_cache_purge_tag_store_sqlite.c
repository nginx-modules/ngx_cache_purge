#include "ngx_cache_purge_tag_store_internal.h"

#if (NGX_LINUX)

static void ngx_http_cache_tag_store_sqlite_close(
    ngx_http_cache_tag_store_t *store);
static ngx_int_t ngx_http_cache_tag_store_sqlite_begin_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_commit_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_rollback_batch(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_replace_file_tags(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    time_t mtime, off_t size, ngx_array_t *tags, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_delete_file(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name, ngx_str_t *path,
    ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_collect_paths_by_tags(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_array_t *tags, ngx_array_t **paths, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_get_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_set_zone_state(
    ngx_http_cache_tag_store_t *store, ngx_str_t *zone_name,
    ngx_http_cache_tag_zone_state_t *state, ngx_log_t *log);
static ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_sqlite_open_one(
    ngx_str_t *path, int flags, ngx_flag_t readonly, ngx_log_t *log,
    ngx_flag_t log_errors);
static ngx_int_t ngx_http_cache_tag_store_sqlite_prepare(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_prepare_one(
    sqlite3 *db, sqlite3_stmt **stmt, const char *sql, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_exec(
    ngx_http_cache_tag_store_t *store, const char *sql, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_sqlite_ensure_schema(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static int ngx_http_cache_tag_store_sqlite_step(sqlite3_stmt *stmt, sqlite3 *db,
        ngx_log_t *log, const char *action);
static void ngx_http_cache_tag_store_sqlite_finalize(
    ngx_http_cache_tag_store_t *store);

static const ngx_http_cache_tag_store_ops_t ngx_http_cache_tag_store_sqlite_ops = {
    ngx_http_cache_tag_store_sqlite_close,
    ngx_http_cache_tag_store_sqlite_begin_batch,
    ngx_http_cache_tag_store_sqlite_commit_batch,
    ngx_http_cache_tag_store_sqlite_rollback_batch,
    ngx_http_cache_tag_store_sqlite_replace_file_tags,
    ngx_http_cache_tag_store_sqlite_delete_file,
    ngx_http_cache_tag_store_sqlite_collect_paths_by_tags,
    ngx_http_cache_tag_store_sqlite_get_zone_state,
    ngx_http_cache_tag_store_sqlite_set_zone_state
};

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_sqlite_open(ngx_http_cache_purge_main_conf_t *pmcf,
                                     ngx_flag_t readonly, ngx_log_t *log) {
    ngx_http_cache_tag_store_t  *store;

    if (pmcf == NULL || pmcf->sqlite_path.len == 0) {
        return NULL;
    }

    if (readonly) {
        store = ngx_http_cache_tag_store_sqlite_open_one(&pmcf->sqlite_path,
                SQLITE_OPEN_READONLY,
                1, log, 0);
        if (store != NULL && ngx_http_cache_tag_store_sqlite_prepare(store, log)
                == NGX_OK) {
            return store;
        }

        ngx_http_cache_tag_store_close(store);
    }

    store = ngx_http_cache_tag_store_sqlite_open_one(&pmcf->sqlite_path,
            SQLITE_OPEN_READWRITE
            | SQLITE_OPEN_CREATE,
            readonly ? 1 : 0, log, 1);
    if (store == NULL) {
        return NULL;
    }

    if (ngx_http_cache_tag_store_sqlite_ensure_schema(store, log) != NGX_OK
            || ngx_http_cache_tag_store_sqlite_prepare(store, log) != NGX_OK) {
        ngx_http_cache_tag_store_close(store);
        return NULL;
    }

    return store;
}

static void
ngx_http_cache_tag_store_sqlite_close(ngx_http_cache_tag_store_t *store) {
    ngx_http_cache_tag_store_sqlite_finalize(store);

    if (store->u.sqlite.db != NULL) {
        sqlite3_close(store->u.sqlite.db);
    }
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_ensure_schema(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    static const char *schema[] = {
        "PRAGMA journal_mode=WAL;",
        "PRAGMA synchronous=NORMAL;",
        "CREATE TABLE IF NOT EXISTS cache_tag_entries ("
        " zone TEXT NOT NULL,"
        " tag TEXT NOT NULL,"
        " path TEXT NOT NULL,"
        " mtime INTEGER NOT NULL,"
        " size INTEGER NOT NULL,"
        " PRIMARY KEY(zone, tag, path)"
        ");",
        "CREATE INDEX IF NOT EXISTS cache_tag_entries_lookup "
        "ON cache_tag_entries(zone, tag);",
        "CREATE TABLE IF NOT EXISTS cache_tag_zones ("
        " zone TEXT PRIMARY KEY,"
        " bootstrap_complete INTEGER NOT NULL DEFAULT 0,"
        " last_bootstrap_at INTEGER NOT NULL DEFAULT 0"
        ");"
    };
    ngx_uint_t  i;

    if (store->u.sqlite.schema_ready) {
        return NGX_OK;
    }

    for (i = 0; i < sizeof(schema) / sizeof(schema[0]); i++) {
        if (ngx_http_cache_tag_store_sqlite_exec(store, schema[i], log) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    store->u.sqlite.schema_ready = 1;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_begin_batch(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    return ngx_http_cache_tag_store_sqlite_exec(store, "BEGIN IMMEDIATE;", log);
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_commit_batch(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    return ngx_http_cache_tag_store_sqlite_exec(store, "COMMIT;", log);
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_rollback_batch(ngx_http_cache_tag_store_t *store,
        ngx_log_t *log) {
    return ngx_http_cache_tag_store_sqlite_exec(store, "ROLLBACK;", log);
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_replace_file_tags(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, time_t mtime, off_t size,
        ngx_array_t *tags, ngx_log_t *log) {
    ngx_str_t  *tag;
    ngx_uint_t  i;
    int         rc;

    if (ngx_http_cache_tag_store_sqlite_delete_file(store, zone_name, path, log)
            != NGX_OK) {
        return NGX_ERROR;
    }

    if (tags == NULL || tags->nelts == 0) {
        return NGX_OK;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        sqlite3_reset(store->u.sqlite.stmt.insert_entry);
        sqlite3_clear_bindings(store->u.sqlite.stmt.insert_entry);
        sqlite3_bind_text(store->u.sqlite.stmt.insert_entry, 1,
                          (const char *) zone_name->data, zone_name->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(store->u.sqlite.stmt.insert_entry, 2,
                          (const char *) tag[i].data, tag[i].len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(store->u.sqlite.stmt.insert_entry, 3,
                          (const char *) path->data, path->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(store->u.sqlite.stmt.insert_entry, 4,
                           (sqlite3_int64) mtime);
        sqlite3_bind_int64(store->u.sqlite.stmt.insert_entry, 5,
                           (sqlite3_int64) size);

        rc = ngx_http_cache_tag_store_sqlite_step(
                 store->u.sqlite.stmt.insert_entry, store->u.sqlite.db, log,
                 "insert");
        if (rc != SQLITE_DONE) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite insert failed: %s",
                          sqlite3_errmsg(store->u.sqlite.db));
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_delete_file(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path,
        ngx_log_t *log) {
    int  rc;

    sqlite3_reset(store->u.sqlite.stmt.delete_file);
    sqlite3_clear_bindings(store->u.sqlite.stmt.delete_file);
    sqlite3_bind_text(store->u.sqlite.stmt.delete_file, 1,
                      (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(store->u.sqlite.stmt.delete_file, 2,
                      (const char *) path->data, path->len,
                      SQLITE_TRANSIENT);

    rc = ngx_http_cache_tag_store_sqlite_step(store->u.sqlite.stmt.delete_file,
            store->u.sqlite.db, log, "delete");
    if (rc != SQLITE_DONE) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite delete failed: %s",
                      sqlite3_errmsg(store->u.sqlite.db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_collect_paths_by_tags(
    ngx_http_cache_tag_store_t *store, ngx_pool_t *pool, ngx_str_t *zone_name,
    ngx_array_t *tags, ngx_array_t **paths, ngx_log_t *log) {
#define NGX_CACHE_TAG_SQLITE_COLLECT_PREFIX \
    "SELECT DISTINCT path FROM cache_tag_entries WHERE zone = ?1 AND tag IN ("

    ngx_array_t    *result;
    ngx_str_t      *tag, *path;
    const u_char   *text;
    ngx_uint_t      i;
    int             rc;
    size_t          len, sql_len;
    sqlite3_stmt   *stmt;
    u_char         *sql, *p;
    ngx_int_t       is_adhoc;

    result = ngx_array_create(pool, 8, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    if (tags == NULL || tags->nelts == 0) {
        *paths = result;
        return NGX_OK;
    }

    tag = tags->elts;
    is_adhoc = (tags->nelts > 1);

    if (!is_adhoc) {
        /* single-tag fast path: reuse pre-prepared statement */
        stmt = store->u.sqlite.stmt.collect_paths;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, (const char *) zone_name->data,
                          zone_name->len, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, (const char *) tag[0].data,
                          tag[0].len, SQLITE_TRANSIENT);

    } else {
        /* multi-tag: build "WHERE zone = ?1 AND tag IN (?2, ?3, ...)" */
        /* Each placeholder is at most "?1001," = 6 chars; +2 for ")\0" */
        sql_len = sizeof(NGX_CACHE_TAG_SQLITE_COLLECT_PREFIX) - 1
                  + tags->nelts * 6 + 2;

        sql = ngx_palloc(pool, sql_len);
        if (sql == NULL) {
            return NGX_ERROR;
        }

        p = ngx_cpymem(sql, NGX_CACHE_TAG_SQLITE_COLLECT_PREFIX,
                       sizeof(NGX_CACHE_TAG_SQLITE_COLLECT_PREFIX) - 1);

        for (i = 0; i < tags->nelts; i++) {
            p = ngx_sprintf(p, "?%ui", (ngx_uint_t)(i + 2));
            if (i + 1 < tags->nelts) {
                *p++ = ',';
            }
        }
        *p++ = ')';
        *p   = '\0';

        if (sqlite3_prepare_v2(store->u.sqlite.db, (const char *) sql, -1,
                               &stmt, NULL) != SQLITE_OK) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite prepare collect_paths failed: %s",
                          sqlite3_errmsg(store->u.sqlite.db));
            return NGX_ERROR;
        }

        sqlite3_bind_text(stmt, 1, (const char *) zone_name->data,
                          zone_name->len, SQLITE_TRANSIENT);

        for (i = 0; i < tags->nelts; i++) {
            sqlite3_bind_text(stmt, (int)(i + 2),
                              (const char *) tag[i].data,
                              tag[i].len, SQLITE_TRANSIENT);
        }
    }

    while ((rc = ngx_http_cache_tag_store_sqlite_step(
                     stmt, store->u.sqlite.db,
                     log, "lookup")) == SQLITE_ROW) {
        text = sqlite3_column_text(stmt, 0);
        if (text == NULL) {
            continue;
        }

        len = ngx_strlen(text);

        path = ngx_array_push(result);
        if (path == NULL) {
            if (is_adhoc) {
                sqlite3_finalize(stmt);
            }
            return NGX_ERROR;
        }

        path->data = ngx_pnalloc(pool, len + 1);
        if (path->data == NULL) {
            if (is_adhoc) {
                sqlite3_finalize(stmt);
            }
            return NGX_ERROR;
        }

        ngx_memcpy(path->data, text, len);
        path->len = len;
        path->data[len] = '\0';
    }

    if (is_adhoc) {
        sqlite3_finalize(stmt);
    }

    if (rc != SQLITE_DONE) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite lookup failed: %s",
                      sqlite3_errmsg(store->u.sqlite.db));
        return NGX_ERROR;
    }

    *paths = result;

    return NGX_OK;

#undef NGX_CACHE_TAG_SQLITE_COLLECT_PREFIX
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_get_zone_state(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log) {
    int  rc;

    state->bootstrap_complete = 0;
    state->last_bootstrap_at = 0;

    sqlite3_reset(store->u.sqlite.stmt.get_zone_state);
    sqlite3_clear_bindings(store->u.sqlite.stmt.get_zone_state);
    sqlite3_bind_text(store->u.sqlite.stmt.get_zone_state, 1,
                      (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);

    rc = ngx_http_cache_tag_store_sqlite_step(store->u.sqlite.stmt.get_zone_state,
            store->u.sqlite.db, log,
            "zone-state read");
    if (rc == SQLITE_ROW) {
        state->bootstrap_complete = sqlite3_column_int(
                                        store->u.sqlite.stmt.get_zone_state, 0)
                                    ? 1 : 0;
        state->last_bootstrap_at = (time_t) sqlite3_column_int64(
                                       store->u.sqlite.stmt.get_zone_state, 1);
        return NGX_OK;
    }

    if (rc == SQLITE_DONE) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite zone-state read failed: %s",
                  sqlite3_errmsg(store->u.sqlite.db));
    return NGX_ERROR;
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_set_zone_state(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name,
        ngx_http_cache_tag_zone_state_t *state,
        ngx_log_t *log) {
    int  rc;

    sqlite3_reset(store->u.sqlite.stmt.set_zone_state);
    sqlite3_clear_bindings(store->u.sqlite.stmt.set_zone_state);
    sqlite3_bind_text(store->u.sqlite.stmt.set_zone_state, 1,
                      (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(store->u.sqlite.stmt.set_zone_state, 2,
                     state->bootstrap_complete ? 1 : 0);
    sqlite3_bind_int64(store->u.sqlite.stmt.set_zone_state, 3,
                       (sqlite3_int64) state->last_bootstrap_at);

    rc = ngx_http_cache_tag_store_sqlite_step(store->u.sqlite.stmt.set_zone_state,
            store->u.sqlite.db, log,
            "zone-state write");
    if (rc != SQLITE_DONE) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite zone-state write failed: %s",
                      sqlite3_errmsg(store->u.sqlite.db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_sqlite_open_one(ngx_str_t *path, int flags,
        ngx_flag_t readonly, ngx_log_t *log,
        ngx_flag_t log_errors) {
    ngx_http_cache_tag_store_t  *store;
    sqlite3                     *db;
    int                          rc;

    db = NULL;
    rc = sqlite3_open_v2((const char *) path->data, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        if (log_errors) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite open failed for \"%V\": %s", path,
                          db != NULL ? sqlite3_errmsg(db) : "unknown");
        }
        if (db != NULL) {
            sqlite3_close(db);
        }
        return NULL;
    }

    store = ngx_pcalloc(ngx_cycle->pool, sizeof(ngx_http_cache_tag_store_t));
    if (store == NULL) {
        sqlite3_close(db);
        return NULL;
    }
    store->ops = &ngx_http_cache_tag_store_sqlite_ops;
    store->backend = NGX_HTTP_CACHE_TAG_BACKEND_SQLITE;
    store->readonly = readonly;
    store->u.sqlite.db = db;

    /* Disable SQLite's internal sleep-based busy handler so that SQLITE_BUSY
     * never calls nanosleep() inside the nginx event loop.  The step function
     * below retries up to five times without sleeping; in WAL mode contention
     * is rare and short-lived, making an immediate spin sufficient. */
    sqlite3_busy_timeout(db, 0);

    /* Raise the bind-parameter limit so large IN-clause queries (e.g.
     * purge by many tags) work.  The default is 999; 32766 is the hard
     * maximum SQLite supports.  This is per-connection and has no effect
     * on other databases. */
    sqlite3_limit(db, SQLITE_LIMIT_VARIABLE_NUMBER, 32766);

    return store;
}

static int
ngx_http_cache_tag_store_sqlite_step(sqlite3_stmt *stmt, sqlite3 *db,
                                     ngx_log_t *log, const char *action) {
    ngx_uint_t  attempt;
    int         rc;

    for (attempt = 0; attempt < 5; attempt++) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            return rc;
        }

        /* Do not sleep here: sleeping inside a timer callback blocks the
         * nginx event loop.  sqlite3_busy_timeout is set to 0 so SQLite
         * itself will not sleep either.  In WAL mode SQLITE_BUSY is rare;
         * spin up to five times and give up rather than stalling the worker. */
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
                       "sqlite %s busy, retry %ui", action, attempt + 1);
    }

    ngx_log_error(NGX_LOG_WARN, log, 0, "sqlite %s remained busy: %s",
                  action, sqlite3_errmsg(db));

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_prepare(ngx_http_cache_tag_store_t *store,
                                        ngx_log_t *log) {
    if (ngx_http_cache_tag_store_sqlite_prepare_one(store->u.sqlite.db,
            &store->u.sqlite.stmt.delete_file,
            "DELETE FROM cache_tag_entries WHERE zone = ?1 AND path = ?2",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_sqlite_prepare_one(store->u.sqlite.db,
            &store->u.sqlite.stmt.insert_entry,
            "INSERT OR REPLACE INTO cache_tag_entries "
            "(zone, tag, path, mtime, size) VALUES (?1, ?2, ?3, ?4, ?5)",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_sqlite_prepare_one(store->u.sqlite.db,
            &store->u.sqlite.stmt.collect_paths,
            "SELECT path FROM cache_tag_entries WHERE zone = ?1 AND tag = ?2",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_sqlite_prepare_one(store->u.sqlite.db,
            &store->u.sqlite.stmt.get_zone_state,
            "SELECT bootstrap_complete, last_bootstrap_at "
            "FROM cache_tag_zones WHERE zone = ?1",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_sqlite_prepare_one(store->u.sqlite.db,
            &store->u.sqlite.stmt.set_zone_state,
            "INSERT OR REPLACE INTO cache_tag_zones "
            "(zone, bootstrap_complete, last_bootstrap_at) "
            "VALUES (?1, ?2, ?3)",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_prepare_one(sqlite3 *db, sqlite3_stmt **stmt,
        const char *sql, ngx_log_t *log) {
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite prepare failed: %s",
                      sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_sqlite_exec(ngx_http_cache_tag_store_t *store,
                                     const char *sql, ngx_log_t *log) {
    char  *errmsg;

    errmsg = NULL;
    if (sqlite3_exec(store->u.sqlite.db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite exec failed: %s",
                      errmsg != NULL ? errmsg : "unknown");
        if (errmsg != NULL) {
            sqlite3_free(errmsg);
        }
        return NGX_ERROR;
    }

    return NGX_OK;
}

static void
ngx_http_cache_tag_store_sqlite_finalize(ngx_http_cache_tag_store_t *store) {
    if (store->u.sqlite.stmt.delete_file != NULL) {
        sqlite3_finalize(store->u.sqlite.stmt.delete_file);
    }

    if (store->u.sqlite.stmt.insert_entry != NULL) {
        sqlite3_finalize(store->u.sqlite.stmt.insert_entry);
    }

    if (store->u.sqlite.stmt.collect_paths != NULL) {
        sqlite3_finalize(store->u.sqlite.stmt.collect_paths);
    }

    if (store->u.sqlite.stmt.get_zone_state != NULL) {
        sqlite3_finalize(store->u.sqlite.stmt.get_zone_state);
    }

    if (store->u.sqlite.stmt.set_zone_state != NULL) {
        sqlite3_finalize(store->u.sqlite.stmt.set_zone_state);
    }
}

#endif
