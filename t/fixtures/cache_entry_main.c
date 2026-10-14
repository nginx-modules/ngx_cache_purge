static void valid_entry(ngx_str_t *path, size_t key_len)
{
    ngx_http_file_cache_header_t header;
    ngx_md5_t md5;
    u_char hash[16], *key;

    memset(contents, 0, sizeof(contents));
    memset(&header, 0, sizeof(header));
    header.version = NGX_HTTP_CACHE_VERSION;
    header.header_start = (u_short) (sizeof(header) + 6 + key_len + 1);
    header.body_start = header.header_start;
    header.etag_len = NGX_HTTP_CACHE_ETAG_LEN;
    memset(header.etag, 'e', sizeof(header.etag));
    key = contents + sizeof(header) + 6;
    memset(key, 'a', key_len);
    key[key_len] = LF;
    header.crc32 = test_crc(key, key_len);
    memcpy(contents, &header, sizeof(header));
    memcpy(contents + sizeof(header), "\nKEY: ", 6);
    length = header.body_start + 4;
    ngx_md5_init(&md5);
    ngx_md5_update(&md5, key, key_len);
    ngx_md5_final(hash, &md5);
    ngx_hex_dump(path->data + path->len - 32, hash, sizeof(hash));
    reads = short_read = fail_read = fail_open = fail_stat = directory = 0;
}

int main(void)
{
    ngx_http_cache_purge_walk_ctx_t ctx;
    ngx_http_file_cache_header_t header;
    ngx_log_t log;
    ngx_str_t path = ngx_string("/cache/00000000000000000000000000000000");
    u_char name[sizeof("/cache/00000000000000000000000000000000")];
    unsigned i, before;

    memcpy(name, path.data, sizeof(name));
    path.data = name;
    memset(&ctx, 0, sizeof(ctx));
    ctx.log = &log;
    valid_entry(&path, 8);
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == 0);
    assert(ctx.entry_key_len == 8 && ctx.header.etag_len == NGX_HTTP_CACHE_ETAG_LEN);
    for (i = 1; i <= 3; i++) {
        valid_entry(&path, 8);
        short_read = i;
        before = closes;
        assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_DECLINED);
        assert(closes == before + 1 && ctx.entry_key_len == 0);
        valid_entry(&path, 8);
        fail_read = i;
        assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_ERROR);
    }
    for (i = 0; i < 8; i++) {
        valid_entry(&path, 8);
        memcpy(&header, contents, sizeof(header));
        switch (i) {
        case 0: header.version++; break;
        case 1: header.header_start = 1; break;
        case 2: header.header_start = sizeof(header) + 6 + 8194; break;
        case 3: header.body_start = header.header_start - 1; break;
        case 4: header.body_start = (u_short) (length + 1); break;
        case 5: header.etag_len++; break;
        case 6: header.vary_len = NGX_HTTP_CACHE_VARY_LEN + 1; break;
        case 7: header.crc32++; break;
        }
        memcpy(contents, &header, sizeof(header));
        assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_DECLINED);
    }
    valid_entry(&path, 8); contents[sizeof(header) + 1] = 'X';
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_DECLINED);
    valid_entry(&path, 8); contents[sizeof(header) + 6 + 8] = 'x';
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_DECLINED);
    valid_entry(&path, 8); name[path.len - 1] ^= 1;
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_DECLINED);
    valid_entry(&path, 8192);
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == 0 && ctx.entry_key_len == 8192);
    valid_entry(&path, 8); directory = 1;
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_DECLINED);
    valid_entry(&path, 8); fail_open = 1; before = closes;
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_ERROR && closes == before);
    valid_entry(&path, 8); fail_stat = 1;
    assert(ngx_http_cache_purge_read_key(&ctx, &path, 0) == NGX_ERROR && closes == before + 1);
    puts("native cache header bounds, short reads, key identity and cleanup checked");
    return 0;
}
