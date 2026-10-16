/*
 * Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
 * Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
 * Copyright (C) 2016-2026, Denys Denysov
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
#include "ngx_cache_purge.h"

#if (NGX_HTTP_CACHE)

typedef struct {
    ngx_uint_t  status;
    ngx_str_t   code;
    ngx_str_t   title;
    ngx_str_t   detail;
} ngx_http_cache_purge_response_t;

#define NGX_PURGE_RESPONSE(s, c, t, d) \
    { s, ngx_string(c), ngx_string(t), ngx_string(d) }

static const ngx_http_cache_purge_response_t ngx_http_cache_purge_responses[] = {
    NGX_PURGE_RESPONSE(200, "purged", "OK", "Matching cache files were deleted."),
    NGX_PURGE_RESPONSE(200, "no_match", "OK", "No matching cache entry was found."),
    NGX_PURGE_RESPONSE(202, "accepted", "Accepted", "Cleanup was accepted; completion is not guaranteed."),
    NGX_PURGE_RESPONSE(200, "invalidated", "OK", "Logical invalidation is complete; physical cleanup is pending."),
    NGX_PURGE_RESPONSE(200, "status", "OK", "Queue status snapshot."),
    NGX_PURGE_RESPONSE(200, "refresh_complete", "OK", "The refresh scan finished; inspect each entry result."),
    NGX_PURGE_RESPONSE(200, "refresh_incomplete", "OK", "Refresh is incomplete; inspect the cursor and entry results."),
    NGX_PURGE_RESPONSE(400, "invalid_selector", "Bad Request", "The selector exceeds the supported key length."),
    NGX_PURGE_RESPONSE(400, "invalid_cursor", "Bad Request", "Provide one cursor containing exactly 32 hexadecimal digits."),
    NGX_PURGE_RESPONSE(405, "method_not_allowed", "Method Not Allowed", "Use a method listed in the Allow header."),
    NGX_PURGE_RESPONSE(403, "access_denied", "Forbidden", "The client is not permitted to purge this cache."),
    NGX_PURGE_RESPONSE(500, "cache_unavailable", "Internal Server Error", "No usable cache zone is configured for this operation."),
    NGX_PURGE_RESPONSE(500, "key_mismatch", "Internal Server Error", "The configured cache key does not satisfy the namespace contract."),
    NGX_PURGE_RESPONSE(500, "delete_failed", "Internal Server Error", "At least one deletion failed; some files may already have been removed."),
    NGX_PURGE_RESPONSE(500, "scan_failed", "Internal Server Error", "The cache scan failed; some files may already have been processed."),
    NGX_PURGE_RESPONSE(500, "internal_error", "Internal Server Error", "The operation could not be completed; consult the server error log.")
};

typedef char ngx_http_cache_purge_catalog_size[
    (sizeof(ngx_http_cache_purge_responses)
     / sizeof(ngx_http_cache_purge_responses[0]) == NGX_PURGE_RESULT_COUNT)
    ? 1 : -1];

static const ngx_http_cache_purge_response_t *
ngx_http_cache_purge_response(ngx_http_request_t *r,
    ngx_http_cache_purge_result_code_t code)
{
    if ((ngx_uint_t) code >= NGX_PURGE_RESULT_COUNT) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_cache_purge: unexpected result code %ui", (ngx_uint_t) code);
        code = NGX_PURGE_RESULT_INTERNAL_ERROR;
    }
    return &ngx_http_cache_purge_responses[code];
}


static ngx_int_t
ngx_http_cache_purge_header(ngx_http_request_t *r, ngx_str_t *name,
    const ngx_str_t *value)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;
    ngx_uint_t       i;

    part = &r->headers_out.headers.part;
    for ( ;; part = part->next) {
        h = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (h[i].key.len == name->len
                && ngx_strncasecmp(h[i].key.data, name->data, name->len) == 0)
            {
                h[i].hash = 1;
                h[i].value = *value;
                return NGX_OK;
            }
        }
        if (part->next == NULL) {
            break;
        }
    }
    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    ngx_memzero(h, sizeof(*h));
    h->hash = 1;
    h->key = *name;
    h->value = *value;
    return NGX_OK;
}


ngx_int_t
ngx_http_cache_purge_response_headers(ngx_http_request_t *r,
    ngx_http_cache_purge_result_code_t code)
{
    const ngx_http_cache_purge_response_t *result;
    ngx_str_t  name = ngx_string("Cache-Purge-Code");
    ngx_str_t  cache_control = ngx_string("Cache-Control");
    ngx_str_t  no_store = ngx_string("no-store");

    result = ngx_http_cache_purge_response(r, code);
    r->headers_out.status = result->status;
    if (ngx_http_cache_purge_header(r, &name, &result->code) != NGX_OK
        || ngx_http_cache_purge_header(r, &cache_control, &no_store) != NGX_OK)
    {
        return NGX_ERROR;
    }
    return NGX_OK;
}


ngx_int_t
ngx_http_cache_purge_method_response(ngx_http_request_t *r, ngx_str_t *allow)
{
    ngx_str_t name = ngx_string("Allow");

    if (ngx_http_cache_purge_header(r, &name, allow) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    return ngx_http_cache_purge_reply(r, NGX_PURGE_RESULT_METHOD_NOT_ALLOWED);
}


/* Keep JSON UTF-8 and XML 1.0 valid even for a binary configured cache key. */
static ngx_uint_t
ngx_http_cache_purge_text_key(ngx_str_t *key)
{
    u_char    *p, *end;
    uint32_t   c;

    p = key->data;
    end = p + key->len;
    while (p < end) {
        c = *p;
        if (c < 0x80) {
            p++;
        } else {
            c = ngx_utf8_decode(&p, (size_t) (end - p));
        }
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)
            || c == 0xfffe || c == 0xffff
            || (c < 0x20 && c != '\t' && c != '\n' && c != '\r'))
        {
            return 0;
        }
    }
    return 1;
}


static uintptr_t
ngx_http_cache_purge_escape_key(u_char *dst, u_char *src, size_t len,
    ngx_uint_t type)
{
    size_t  extra;

    if (len == 0) {
        return (uintptr_t) dst;
    }

    if (type == NGX_CACHE_PURGE_RESPONSE_TYPE_JSON) {
        return ngx_escape_json(dst, src, len);
    }

    if (type == NGX_CACHE_PURGE_RESPONSE_TYPE_HTML) {
        return ngx_escape_html(dst, src, len);
    }

    if (type == NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT) {
        return dst ? (uintptr_t) ngx_cpymem(dst, src, len) : 0;
    }

    /* Split a CDATA terminator without changing the decoded key. */
    extra = 0;
    while (len != 0) {
        if (len >= 3 && src[0] == ']' && src[1] == ']' && src[2] == '>') {
            if (dst != NULL) {
                dst = ngx_cpymem(dst, "]]]]><![CDATA[>", 15);
            }
            extra += 12;
            src += 3;
            len -= 3;

        } else {
            if (dst != NULL) {
                *dst++ = *src;
            }
            src++;
            len--;
        }
    }

    return dst ? (uintptr_t) dst : extra;
}


ngx_int_t
ngx_http_cache_purge_reply(ngx_http_request_t *r,
    ngx_http_cache_purge_result_code_t code)
{
    ngx_http_cache_purge_result_t result;

    ngx_memzero(&result, sizeof(result));
    result.code = code;
    return ngx_http_cache_purge_send_result(r, &result);
}


ngx_int_t
ngx_http_cache_purge_send_result(ngx_http_request_t *r,
    ngx_http_cache_purge_result_t *result)
{
    const ngx_http_cache_purge_response_t *desc;
    ngx_http_cache_purge_loc_conf_t *conf;
    ngx_str_t key = ngx_null_string;
    ngx_str_t encoding = ngx_string("utf-8");
    ngx_str_t raw;
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_uint_t type, complete, pending;
    ngx_int_t rc;
    size_t len;

    desc = ngx_http_cache_purge_response(r, result->code);
    conf = ngx_http_get_module_loc_conf(r, ngx_http_cache_purge_module);
    type = conf->response_type;
    if (conf->queue_status_cache_path.data != NULL) {
        type = NGX_CACHE_PURGE_RESPONSE_TYPE_JSON;
    }
#if (NGX_HTTP_PROXY)
    if (conf->refresh) {
        type = NGX_CACHE_PURGE_RESPONSE_TYPE_JSON;
    }
#endif
    if (ngx_http_cache_purge_response_headers(r, result->code) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    if (type == NGX_CACHE_PURGE_RESPONSE_TYPE_NONE) {
        if (desc->status == NGX_HTTP_OK) {
            r->headers_out.status = NGX_HTTP_NO_CONTENT;
        }
        r->headers_out.content_length_n = 0;
        r->header_only = 1;
        return ngx_http_send_header(r);
    }
    if (r->cache != NULL && r->cache->keys.nelts != 0) {
        key = ((ngx_str_t *) r->cache->keys.elts)[0];
    }
    if (desc->status >= 400) {
        key.len = 0;
        type = NGX_CACHE_PURGE_RESPONSE_TYPE_JSON;
    }
    if (key.len > ((size_t) -1 - 2048) / 8) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    if (key.len && !ngx_http_cache_purge_text_key(&key)) {
        raw = key;
        key.len = ngx_base64_encoded_length(raw.len);
        key.data = ngx_pnalloc(r->pool, key.len);
        if (key.data == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        ngx_encode_base64(&key, &raw);
        ngx_str_set(&encoding, "base64");
    }
    len = 2048 + key.len * 6;
    b = ngx_create_temp_buf(r->pool, len);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    complete = desc->status == NGX_HTTP_OK;
    pending = result->code == NGX_PURGE_RESULT_ACCEPTED
              || result->code == NGX_PURGE_RESULT_INVALIDATED;
    if (desc->status >= 400) {
        ngx_str_set(&r->headers_out.content_type, "application/problem+json");
        b->last = ngx_sprintf(b->last,
            "{\"type\":\"about:blank\",\"title\":\"%V\",\"status\":%ui,"
            "\"detail\":\"%V\",\"code\":\"%V\"",
            &desc->title, desc->status, &desc->detail, &desc->code);
    } else {
        switch (type) {
        case NGX_CACHE_PURGE_RESPONSE_TYPE_HTML:
            ngx_str_set(&r->headers_out.content_type, "text/html");
            b->last = ngx_sprintf(b->last,
                "<!doctype html><html><head><title>Cache Purge</title></head>"
                "<body><h1>Cache Purge</h1><p>Key: ");
            break;
        case NGX_CACHE_PURGE_RESPONSE_TYPE_XML:
            ngx_str_set(&r->headers_out.content_type, "application/xml");
            b->last = ngx_sprintf(b->last,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response><Key><![CDATA[");
            break;
        case NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT:
            ngx_str_set(&r->headers_out.content_type, "text/plain");
            b->last = ngx_sprintf(b->last, "Key: ");
            break;
        default:
            type = NGX_CACHE_PURGE_RESPONSE_TYPE_JSON;
            ngx_str_set(&r->headers_out.content_type, "application/json");
            b->last = ngx_sprintf(b->last, "{\"key\":\"");
        }
        b->last = (u_char *) ngx_http_cache_purge_escape_key(b->last,
                                                     key.data, key.len, type);
        switch (type) {
        case NGX_CACHE_PURGE_RESPONSE_TYPE_HTML:
            b->last = ngx_sprintf(b->last,
                "</p><p>Code: %V</p><p>Key encoding: %V</p></body></html>",
                &desc->code, &encoding);
            break;
        case NGX_CACHE_PURGE_RESPONSE_TYPE_XML:
            b->last = ngx_sprintf(b->last,
                "]]></Key><Code>%V</Code><KeyEncoding>%V</KeyEncoding></response>",
                &desc->code, &encoding);
            break;
        case NGX_CACHE_PURGE_RESPONSE_TYPE_TEXT:
            b->last = ngx_sprintf(b->last, "\nCode: %V\nKey encoding: %V\n",
                                 &desc->code, &encoding);
            break;
        default:
            b->last = ngx_sprintf(b->last,
                "\",\"key_encoding\":\"%V\",\"code\":\"%V\",\"complete\":%s,"
                "\"cleanup_pending\":%s", &encoding, &desc->code,
                complete ? "true" : "false", pending ? "true" : "false");
        }
    }
    if (type == NGX_CACHE_PURGE_RESPONSE_TYPE_JSON) {
        if (result->counted) {
            b->last = ngx_sprintf(b->last,
                ",\"deleted\":%ui,\"delete_errors\":%ui,\"scan_errors\":%ui",
                result->deleted, result->delete_errors, result->scan_errors);
        }
        *b->last++ = '}';
    }
    b->last_buf = (r == r->main);
    b->last_in_chain = 1;
    r->headers_out.content_length_n = b->last - b->pos;
    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }
    out.buf = b;
    out.next = NULL;
    return ngx_http_output_filter(r, &out);
}

#endif
