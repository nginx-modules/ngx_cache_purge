#include "ngx_cache_purge_tag.h"

#if (NGX_LINUX)

static ngx_http_cache_tag_store_runtime_t ngx_http_cache_tag_store_runtime;

static ngx_http_cache_tag_store_t *ngx_http_cache_tag_store_open(
    ngx_str_t *path, int flags, ngx_flag_t readonly, ngx_log_t *log,
    ngx_flag_t log_errors);
static ngx_int_t ngx_http_cache_tag_store_prepare(
    ngx_http_cache_tag_store_t *store, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_prepare_one(
    sqlite3 *db, sqlite3_stmt **stmt, const char *sql, ngx_log_t *log);
static ngx_int_t ngx_http_cache_tag_store_exec(
    ngx_http_cache_tag_store_t *store, const char *sql, ngx_log_t *log);
static int ngx_http_cache_tag_store_step(sqlite3_stmt *stmt, sqlite3 *db,
        ngx_log_t *log, const char *action);
static void ngx_http_cache_tag_store_finalize(
    ngx_http_cache_tag_store_t *store);
static ngx_int_t ngx_http_cache_tag_push_unique(ngx_pool_t *pool,
        ngx_array_t *tags, u_char *data, size_t len);
static ngx_int_t ngx_http_cache_tag_path_known(ngx_str_t *path);
static ngx_int_t ngx_http_cache_tag_parse_file(ngx_pool_t *pool,
        ngx_str_t *path, ngx_array_t *headers, ngx_array_t **tags, time_t *mtime,
        off_t *size, ngx_log_t *log);

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_open_writer(ngx_str_t *path, ngx_log_t *log) {
    ngx_http_cache_tag_store_t  *store;

    store = ngx_http_cache_tag_store_open(path,
                                          SQLITE_OPEN_READWRITE
                                          | SQLITE_OPEN_CREATE,
                                          0, log, 1);
    if (store == NULL) {
        return NULL;
    }

    if (ngx_http_cache_tag_store_ensure_schema(store, log) != NGX_OK) {
        ngx_http_cache_tag_store_close(store);
        return NULL;
    }

    if (ngx_http_cache_tag_store_prepare(store, log) != NGX_OK) {
        ngx_http_cache_tag_store_close(store);
        return NULL;
    }

    return store;
}

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_open_reader(ngx_str_t *path, ngx_log_t *log) {
    ngx_http_cache_tag_store_t  *store;

    store = ngx_http_cache_tag_store_open(path, SQLITE_OPEN_READONLY, 1, log, 0);
    if (store != NULL && ngx_http_cache_tag_store_prepare(store, log) == NGX_OK) {
        return store;
    }

    ngx_http_cache_tag_store_close(store);

    store = ngx_http_cache_tag_store_open(path,
                                          SQLITE_OPEN_READWRITE
                                          | SQLITE_OPEN_CREATE,
                                          0, log, 1);
    if (store == NULL) {
        return NULL;
    }

    if (ngx_http_cache_tag_store_ensure_schema(store, log) != NGX_OK) {
        ngx_http_cache_tag_store_close(store);
        return NULL;
    }

    if (ngx_http_cache_tag_store_prepare(store, log) != NGX_OK) {
        ngx_http_cache_tag_store_close(store);
        return NULL;
    }

    return store;
}

void
ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_t *store) {
    if (store == NULL) {
        return;
    }

    ngx_http_cache_tag_store_finalize(store);

    if (store->db != NULL) {
        sqlite3_close(store->db);
    }

    ngx_free(store);
}

ngx_int_t
ngx_http_cache_tag_store_ensure_schema(ngx_http_cache_tag_store_t *store,
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

    if (store == NULL) {
        return NGX_ERROR;
    }

    if (store->schema_ready) {
        return NGX_OK;
    }

    for (i = 0; i < sizeof(schema) / sizeof(schema[0]); i++) {
        if (ngx_http_cache_tag_store_exec(store, schema[i], log) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    store->schema_ready = 1;

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_store_begin_batch(ngx_http_cache_tag_store_t *store,
                                     ngx_log_t *log) {
    return ngx_http_cache_tag_store_exec(store, "BEGIN IMMEDIATE;", log);
}

ngx_int_t
ngx_http_cache_tag_store_commit_batch(ngx_http_cache_tag_store_t *store,
                                      ngx_log_t *log) {
    return ngx_http_cache_tag_store_exec(store, "COMMIT;", log);
}

ngx_int_t
ngx_http_cache_tag_store_rollback_batch(ngx_http_cache_tag_store_t *store,
                                        ngx_log_t *log) {
    return ngx_http_cache_tag_store_exec(store, "ROLLBACK;", log);
}

ngx_int_t
ngx_http_cache_tag_store_replace_file_tags(ngx_http_cache_tag_store_t *store,
        ngx_str_t *zone_name, ngx_str_t *path, time_t mtime, off_t size,
        ngx_array_t *tags, ngx_log_t *log) {
    ngx_str_t  *tag;
    ngx_uint_t  i;
    int         rc;

    if (ngx_http_cache_tag_store_delete_file(store, zone_name, path, log)
            != NGX_OK) {
        return NGX_ERROR;
    }

    if (tags == NULL || tags->nelts == 0) {
        return NGX_OK;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        sqlite3_reset(store->stmt.insert_entry);
        sqlite3_clear_bindings(store->stmt.insert_entry);
        sqlite3_bind_text(store->stmt.insert_entry, 1,
                          (const char *) zone_name->data, zone_name->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(store->stmt.insert_entry, 2,
                          (const char *) tag[i].data, tag[i].len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(store->stmt.insert_entry, 3,
                          (const char *) path->data, path->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(store->stmt.insert_entry, 4, (sqlite3_int64) mtime);
        sqlite3_bind_int64(store->stmt.insert_entry, 5, (sqlite3_int64) size);

        rc = ngx_http_cache_tag_store_step(store->stmt.insert_entry, store->db,
                                           log, "insert");
        if (rc != SQLITE_DONE) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite insert failed: %s",
                          sqlite3_errmsg(store->db));
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_store_delete_file(ngx_http_cache_tag_store_t *store,
                                     ngx_str_t *zone_name, ngx_str_t *path,
                                     ngx_log_t *log) {
    int  rc;

    sqlite3_reset(store->stmt.delete_file);
    sqlite3_clear_bindings(store->stmt.delete_file);
    sqlite3_bind_text(store->stmt.delete_file, 1,
                      (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(store->stmt.delete_file, 2,
                      (const char *) path->data, path->len,
                      SQLITE_TRANSIENT);

    rc = ngx_http_cache_tag_store_step(store->stmt.delete_file, store->db, log,
                                       "delete");
    if (rc != SQLITE_DONE) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite delete failed: %s",
                      sqlite3_errmsg(store->db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_store_collect_paths_by_tags(ngx_http_cache_tag_store_t *store,
        ngx_pool_t *pool, ngx_str_t *zone_name, ngx_array_t *tags,
        ngx_array_t **paths, ngx_log_t *log) {
    ngx_array_t    *result;
    ngx_str_t      *tag, *path;
    const u_char   *text;
    ngx_uint_t      i, j;
    int             rc;
    size_t          len;

    result = ngx_array_create(pool, 8, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        sqlite3_reset(store->stmt.collect_paths);
        sqlite3_clear_bindings(store->stmt.collect_paths);
        sqlite3_bind_text(store->stmt.collect_paths, 1,
                          (const char *) zone_name->data, zone_name->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(store->stmt.collect_paths, 2,
                          (const char *) tag[i].data, tag[i].len,
                          SQLITE_TRANSIENT);

        while ((rc = ngx_http_cache_tag_store_step(store->stmt.collect_paths,
                     store->db, log, "lookup")) == SQLITE_ROW) {
            text = sqlite3_column_text(store->stmt.collect_paths, 0);
            if (text == NULL) {
                continue;
            }

            len = ngx_strlen(text);
            path = result->elts;
            for (j = 0; j < result->nelts; j++) {
                if (path[j].len == len
                        && ngx_strncmp(path[j].data, text, len) == 0) {
                    break;
                }
            }

            if (j != result->nelts) {
                continue;
            }

            path = ngx_array_push(result);
            if (path == NULL) {
                return NGX_ERROR;
            }

            path->data = ngx_pnalloc(pool, len + 1);
            if (path->data == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(path->data, text, len);
            path->len = len;
            path->data[len] = '\0';
        }

        if (rc != SQLITE_DONE) {
            ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite lookup failed: %s",
                          sqlite3_errmsg(store->db));
            return NGX_ERROR;
        }
    }

    *paths = result;

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_store_get_zone_state(ngx_http_cache_tag_store_t *store,
                                        ngx_str_t *zone_name,
                                        ngx_http_cache_tag_zone_state_t *state,
                                        ngx_log_t *log) {
    int  rc;

    state->bootstrap_complete = 0;
    state->last_bootstrap_at = 0;

    sqlite3_reset(store->stmt.get_zone_state);
    sqlite3_clear_bindings(store->stmt.get_zone_state);
    sqlite3_bind_text(store->stmt.get_zone_state, 1,
                      (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);

    rc = ngx_http_cache_tag_store_step(store->stmt.get_zone_state, store->db,
                                       log, "zone-state read");
    if (rc == SQLITE_ROW) {
        state->bootstrap_complete = sqlite3_column_int(store->stmt.get_zone_state,
                                    0) ? 1 : 0;
        state->last_bootstrap_at = (time_t) sqlite3_column_int64(
                                       store->stmt.get_zone_state, 1);
        return NGX_OK;
    }

    if (rc == SQLITE_DONE) {
        return NGX_OK;
    }

    ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite zone-state read failed: %s",
                  sqlite3_errmsg(store->db));
    return NGX_ERROR;
}

ngx_int_t
ngx_http_cache_tag_store_set_zone_state(ngx_http_cache_tag_store_t *store,
                                        ngx_str_t *zone_name,
                                        ngx_http_cache_tag_zone_state_t *state,
                                        ngx_log_t *log) {
    int  rc;

    sqlite3_reset(store->stmt.set_zone_state);
    sqlite3_clear_bindings(store->stmt.set_zone_state);
    sqlite3_bind_text(store->stmt.set_zone_state, 1,
                      (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(store->stmt.set_zone_state, 2,
                     state->bootstrap_complete ? 1 : 0);
    sqlite3_bind_int64(store->stmt.set_zone_state, 3,
                       (sqlite3_int64) state->last_bootstrap_at);

    rc = ngx_http_cache_tag_store_step(store->stmt.set_zone_state, store->db,
                                       log, "zone-state write");
    if (rc != SQLITE_DONE) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite zone-state write failed: %s",
                      sqlite3_errmsg(store->db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_store_process_file(ngx_http_cache_tag_store_t *store,
                                      ngx_str_t *zone_name, ngx_str_t *path,
                                      ngx_array_t *headers,
                                      ngx_log_t *log) {
    ngx_pool_t   *pool;
    ngx_array_t  *tags;
    time_t        mtime;
    off_t         size;
    ngx_int_t     rc;

    pool = ngx_create_pool(4096, log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    if (headers == NULL || headers->nelts == 0) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_tag_store_delete_file(store, zone_name, path, log);
    }

    if (ngx_http_cache_tag_parse_file(pool, path, headers, &tags, &mtime, &size,
                                      log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_tag_store_delete_file(store, zone_name, path, log);
    }

    rc = ngx_http_cache_tag_store_replace_file_tags(store, zone_name, path,
            mtime, size, tags, log);
    ngx_destroy_pool(pool);

    return rc;
}

ngx_int_t
ngx_http_cache_tag_store_runtime_init(ngx_cycle_t *cycle,
                                      ngx_http_cache_purge_main_conf_t *pmcf,
                                      ngx_flag_t owner) {
    ngx_memzero(&ngx_http_cache_tag_store_runtime,
                sizeof(ngx_http_cache_tag_store_runtime));

    ngx_http_cache_tag_store_runtime.cycle = cycle;
    ngx_http_cache_tag_store_runtime.owner = owner;

    if (pmcf->sqlite_path.len == 0) {
        return NGX_OK;
    }

    if (!owner) {
        return NGX_OK;
    }

    ngx_http_cache_tag_store_runtime.writer =
        ngx_http_cache_tag_store_open_writer(&pmcf->sqlite_path, cycle->log);

    return ngx_http_cache_tag_store_runtime.writer != NULL ? NGX_OK : NGX_ERROR;
}

void
ngx_http_cache_tag_store_runtime_shutdown(void) {
    ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_runtime.writer);
    ngx_http_cache_tag_store_close(ngx_http_cache_tag_store_runtime.reader);

    ngx_memzero(&ngx_http_cache_tag_store_runtime,
                sizeof(ngx_http_cache_tag_store_runtime));
}

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_writer(void) {
    return ngx_http_cache_tag_store_runtime.writer;
}

ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_reader(ngx_http_cache_purge_main_conf_t *pmcf,
                                ngx_log_t *log) {
    if (ngx_http_cache_tag_store_runtime.reader != NULL) {
        return ngx_http_cache_tag_store_runtime.reader;
    }

    ngx_http_cache_tag_store_runtime.reader =
        ngx_http_cache_tag_store_open_reader(&pmcf->sqlite_path, log);

    return ngx_http_cache_tag_store_runtime.reader;
}

static ngx_http_cache_tag_store_t *
ngx_http_cache_tag_store_open(ngx_str_t *path, int flags, ngx_flag_t readonly,
                              ngx_log_t *log, ngx_flag_t log_errors) {
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

    store = ngx_alloc(sizeof(ngx_http_cache_tag_store_t), log);
    if (store == NULL) {
        sqlite3_close(db);
        return NULL;
    }

    ngx_memzero(store, sizeof(ngx_http_cache_tag_store_t));
    store->db = db;
    store->readonly = readonly;
    store->schema_ready = 0;

    sqlite3_busy_timeout(db, 5000);

    return store;
}

static int
ngx_http_cache_tag_store_step(sqlite3_stmt *stmt, sqlite3 *db, ngx_log_t *log,
                              const char *action) {
    ngx_uint_t  attempt;
    int         rc;

    for (attempt = 0; attempt < 5; attempt++) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            return rc;
        }

        if (attempt < 4) {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
                           "sqlite %s busy, retry %ui", action, attempt + 1);
            sqlite3_sleep((int)((attempt + 1) * 10));
        }
    }

    ngx_log_error(NGX_LOG_WARN, log, 0, "sqlite %s remained busy: %s",
                  action, sqlite3_errmsg(db));

    return rc;
}

static ngx_int_t
ngx_http_cache_tag_store_prepare(ngx_http_cache_tag_store_t *store,
                                 ngx_log_t *log) {
    if (ngx_http_cache_tag_store_prepare_one(store->db, &store->stmt.delete_file,
            "DELETE FROM cache_tag_entries WHERE zone = ?1 AND path = ?2",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_prepare_one(store->db, &store->stmt.insert_entry,
            "INSERT OR REPLACE INTO cache_tag_entries "
            "(zone, tag, path, mtime, size) VALUES (?1, ?2, ?3, ?4, ?5)",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_prepare_one(store->db,
            &store->stmt.collect_paths,
            "SELECT path FROM cache_tag_entries WHERE zone = ?1 AND tag = ?2",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_prepare_one(store->db,
            &store->stmt.get_zone_state,
            "SELECT bootstrap_complete, last_bootstrap_at "
            "FROM cache_tag_zones WHERE zone = ?1",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_http_cache_tag_store_prepare_one(store->db,
            &store->stmt.set_zone_state,
            "INSERT OR REPLACE INTO cache_tag_zones "
            "(zone, bootstrap_complete, last_bootstrap_at) "
            "VALUES (?1, ?2, ?3)",
            log) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_prepare_one(sqlite3 *db, sqlite3_stmt **stmt,
                                     const char *sql, ngx_log_t *log) {
    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "sqlite prepare failed: %s",
                      sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_store_exec(ngx_http_cache_tag_store_t *store,
                              const char *sql, ngx_log_t *log) {
    char  *errmsg;

    errmsg = NULL;
    if (sqlite3_exec(store->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
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
ngx_http_cache_tag_store_finalize(ngx_http_cache_tag_store_t *store) {
    if (store->stmt.delete_file != NULL) {
        sqlite3_finalize(store->stmt.delete_file);
    }

    if (store->stmt.insert_entry != NULL) {
        sqlite3_finalize(store->stmt.insert_entry);
    }

    if (store->stmt.collect_paths != NULL) {
        sqlite3_finalize(store->stmt.collect_paths);
    }

    if (store->stmt.get_zone_state != NULL) {
        sqlite3_finalize(store->stmt.get_zone_state);
    }

    if (store->stmt.set_zone_state != NULL) {
        sqlite3_finalize(store->stmt.set_zone_state);
    }
}

ngx_int_t
ngx_http_cache_tag_extract_tokens(ngx_pool_t *pool, u_char *value, size_t len,
                                  ngx_array_t *tags) {
    size_t  i, start, end;

    i = 0;
    while (i < len) {
        while (i < len && (value[i] == ' ' || value[i] == '\t'
                           || value[i] == '\r' || value[i] == '\n'
                           || value[i] == ',')) {
            i++;
        }

        start = i;

        while (i < len && value[i] != ' ' && value[i] != '\t'
                && value[i] != '\r' && value[i] != '\n'
                && value[i] != ',') {
            i++;
        }

        end = i;
        if (end > start
                && ngx_http_cache_tag_push_unique(pool, tags, value + start,
                        end - start) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_push_unique(ngx_pool_t *pool, ngx_array_t *tags,
                               u_char *data, size_t len) {
    ngx_str_t   *tag;
    ngx_uint_t   i;

    if (len == 0) {
        return NGX_OK;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        if (tag[i].len == len
                && ngx_strncasecmp(tag[i].data, data, len) == 0) {
            return NGX_OK;
        }
    }

    tag = ngx_array_push(tags);
    if (tag == NULL) {
        return NGX_ERROR;
    }

    tag->data = ngx_pnalloc(pool, len);
    if (tag->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(tag->data, data, len);
    tag->len = len;

    return NGX_OK;
}

static ngx_int_t
ngx_http_cache_tag_path_known(ngx_str_t *path) {
    u_char  *p;

    p = path->data + path->len;
    while (p > path->data && p[-1] != '/') {
        p--;
    }

    return (size_t)(path->data + path->len - p) == 2 * NGX_HTTP_CACHE_KEY_LEN;
}

static ngx_int_t
ngx_http_cache_tag_parse_file(ngx_pool_t *pool, ngx_str_t *path,
                              ngx_array_t *headers, ngx_array_t **tags,
                              time_t *mtime, off_t *size, ngx_log_t *log) {
    ngx_file_info_t  fi;
    ngx_file_t       file;
    u_char          *buf;
    ssize_t          n;
    size_t           max_read, i, j, line_end, value_start;
    ngx_array_t     *result;
    ngx_str_t       *header;

    if (!ngx_http_cache_tag_path_known(path)) {
        return NGX_DECLINED;
    }

    if (ngx_file_info(path->data, &fi) == NGX_FILE_ERROR) {
        return NGX_DECLINED;
    }

    max_read = (size_t) ngx_min((off_t) 65536, ngx_file_size(&fi));
    if (max_read == 0) {
        return NGX_DECLINED;
    }

    buf = ngx_pnalloc(pool, max_read);
    if (buf == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = *path;
    file.log = log;
    file.fd = ngx_open_file(path->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (file.fd == NGX_INVALID_FILE) {
        return NGX_DECLINED;
    }

    n = ngx_read_file(&file, buf, max_read, 0);
    ngx_close_file(file.fd);

    if (n == NGX_ERROR || n <= 0) {
        return NGX_DECLINED;
    }

    result = ngx_array_create(pool, 4, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    header = headers->elts;
    for (i = 0; i < (size_t) n; i++) {
        if (i != 0 && buf[i - 1] != '\n') {
            continue;
        }

        for (j = 0; j < headers->nelts; j++) {
            if (i + header[j].len + 1 >= (size_t) n) {
                continue;
            }

            if (ngx_strncasecmp(buf + i, header[j].data, header[j].len) != 0
                    || buf[i + header[j].len] != ':') {
                continue;
            }

            value_start = i + header[j].len + 1;
            while (value_start < (size_t) n
                    && (buf[value_start] == ' ' || buf[value_start] == '\t')) {
                value_start++;
            }

            line_end = value_start;
            while (line_end < (size_t) n && buf[line_end] != '\n'
                    && buf[line_end] != '\r') {
                line_end++;
            }

            if (ngx_http_cache_tag_extract_tokens(pool, buf + value_start,
                                                  line_end - value_start,
                                                  result) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    *mtime = ngx_file_mtime(&fi);
    *size = ngx_file_size(&fi);
    *tags = result;

    return NGX_OK;
}

#endif
