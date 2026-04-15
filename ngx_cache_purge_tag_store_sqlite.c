#include "ngx_cache_purge_tag.h"

#if (NGX_LINUX)

static ngx_int_t ngx_http_cache_tag_push_unique(ngx_pool_t *pool,
        ngx_array_t *tags, u_char *data, size_t len);
static ngx_int_t ngx_http_cache_tag_path_known(ngx_str_t *path);
static ngx_int_t ngx_http_cache_tag_parse_file(ngx_pool_t *pool,
        ngx_str_t *path, ngx_array_t *headers, ngx_array_t **tags,
        time_t *mtime, off_t *size, ngx_log_t *log);

sqlite3 *
ngx_http_cache_tag_db_open(ngx_str_t *path, int flags, ngx_log_t *log) {
    sqlite3  *db;
    int       rc;

    db = NULL;
    rc = sqlite3_open_v2((const char *) path->data, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite open failed for \"%V\": %s", path,
                      db != NULL ? sqlite3_errmsg(db) : "unknown");
        if (db != NULL) {
            sqlite3_close(db);
        }
        return NULL;
    }

    return db;
}

ngx_int_t
ngx_http_cache_tag_sqlite_init(sqlite3 *db, ngx_log_t *log) {
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
        "ON cache_tag_entries(zone, tag);"
    };
    char        *errmsg;
    ngx_uint_t   i;

    for (i = 0; i < sizeof(schema) / sizeof(schema[0]); i++) {
        errmsg = NULL;
        if (sqlite3_exec(db, schema[i], NULL, NULL, &errmsg) != SQLITE_OK) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite schema init failed: %s",
                          errmsg != NULL ? errmsg : "unknown");
            if (errmsg != NULL) {
                sqlite3_free(errmsg);
            }
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_db_delete_file(sqlite3 *db, ngx_str_t *zone_name,
                                  ngx_str_t *path, ngx_log_t *log) {
    sqlite3_stmt  *stmt;
    int            rc;

    stmt = NULL;
    rc = sqlite3_prepare_v2(db,
                            "DELETE FROM cache_tag_entries "
                            "WHERE zone = ?1 AND path = ?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite prepare delete failed: %s",
                      sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    sqlite3_bind_text(stmt, 1, (const char *) zone_name->data, zone_name->len,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, (const char *) path->data, path->len,
                      SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite delete failed: %s", sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_db_replace(sqlite3 *db, ngx_str_t *zone_name, ngx_str_t *path,
                              time_t mtime, off_t size, ngx_array_t *tags,
                              ngx_log_t *log) {
    sqlite3_stmt  *stmt;
    ngx_str_t     *tag;
    ngx_uint_t     i;
    int            rc;

    if (ngx_http_cache_tag_db_delete_file(db, zone_name, path, log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (tags == NULL || tags->nelts == 0) {
        return NGX_OK;
    }

    rc = sqlite3_prepare_v2(db,
                            "INSERT OR REPLACE INTO cache_tag_entries "
                            "(zone, tag, path, mtime, size) "
                            "VALUES (?1, ?2, ?3, ?4, ?5)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite prepare insert failed: %s",
                      sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, (const char *) zone_name->data, zone_name->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, (const char *) tag[i].data, tag[i].len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, (const char *) path->data, path->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64) mtime);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64) size);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite insert failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return NGX_ERROR;
        }
    }

    sqlite3_finalize(stmt);

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_db_collect_paths(sqlite3 *db, ngx_pool_t *pool,
                                    ngx_str_t *zone_name, ngx_array_t *tags,
                                    ngx_array_t **paths, ngx_log_t *log) {
    sqlite3_stmt  *stmt;
    ngx_array_t   *result;
    ngx_str_t     *tag;
    const u_char  *text;
    ngx_str_t     *path;
    ngx_uint_t     i, j;
    int            rc;
    size_t         len;

    result = ngx_array_create(pool, 8, sizeof(ngx_str_t));
    if (result == NULL) {
        return NGX_ERROR;
    }

    rc = sqlite3_prepare_v2(db,
                            "SELECT path FROM cache_tag_entries "
                            "WHERE zone = ?1 AND tag = ?2",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "sqlite prepare lookup failed: %s", sqlite3_errmsg(db));
        return NGX_ERROR;
    }

    tag = tags->elts;
    for (i = 0; i < tags->nelts; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, (const char *) zone_name->data, zone_name->len,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, (const char *) tag[i].data, tag[i].len,
                          SQLITE_TRANSIENT);

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            text = sqlite3_column_text(stmt, 0);
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
                sqlite3_finalize(stmt);
                return NGX_ERROR;
            }

            path->data = ngx_pnalloc(pool, len + 1);
            if (path->data == NULL) {
                sqlite3_finalize(stmt);
                return NGX_ERROR;
            }

            ngx_memcpy(path->data, text, len);
            path->len = len;
            path->data[len] = '\0';
        }

        if (rc != SQLITE_DONE) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "sqlite lookup failed: %s", sqlite3_errmsg(db));
            sqlite3_finalize(stmt);
            return NGX_ERROR;
        }
    }

    sqlite3_finalize(stmt);
    *paths = result;

    return NGX_OK;
}

ngx_int_t
ngx_http_cache_tag_extract_tokens(ngx_pool_t *pool, u_char *value, size_t len,
                                  ngx_array_t *tags) {
    size_t   i, start, end;

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

ngx_int_t
ngx_http_cache_tag_process_file(sqlite3 *db, ngx_str_t *zone_name, ngx_str_t *path,
                                ngx_log_t *log) {
    ngx_array_t  *headers;
    ngx_array_t  *tags;
    ngx_str_t    *header;
    time_t        mtime;
    off_t         size;
    ngx_int_t     rc;
    ngx_pool_t   *pool;

    pool = ngx_create_pool(4096, log);
    if (pool == NULL) {
        return NGX_ERROR;
    }

    headers = ngx_array_create(pool, 2, sizeof(ngx_str_t));
    if (headers == NULL) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    header = ngx_array_push(headers);
    if (header == NULL) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }
    ngx_str_set(header, "Surrogate-Key");

    header = ngx_array_push(headers);
    if (header == NULL) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }
    ngx_str_set(header, "Cache-Tag");

    if (ngx_http_cache_tag_parse_file(pool, path, headers,
                                      &tags, &mtime, &size, log) != NGX_OK) {
        ngx_destroy_pool(pool);
        return ngx_http_cache_tag_db_delete_file(db, zone_name, path, log);
    }

    rc = ngx_http_cache_tag_db_replace(db, zone_name, path, mtime, size,
                                       tags, log);
    ngx_destroy_pool(pool);

    return rc;
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
