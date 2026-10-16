[ngx_cache_purge](../README.md)

<a id="sample-configurations"></a>

# Configuration examples

The first five blocks are alternative complete `nginx.conf` examples. The Vary block adds handling to an existing `http` section. The last example demonstrates the opt-in v4 generation-key and REFRESH flow. NGINX listens locally on port 8080; `catalog_backend` names the application at `127.0.0.1:8081`. `shop.example.com` is its HTTP Host. Replace the addresses and writable cache directory for your deployment.

## In this guide

- [Choosing a cache operation](#choosing-a-mode)
- [Basic: inline PURGE method](#basic--inline-purge-method)
- [Basic: separate PURGE location](#basic--separate-purge-location)
- [Partial-key PURGE](#partial-key-purge)
- [Background cleanup queue](#background-queue-enabled)
- [Purge all entries](#purge-all-entries)
- [Response format overrides](#json-responses-with-per-location-override)
- [Vary-aware purge](#vary-aware-purge)
- [Generation-key PURGE and REFRESH: product catalog](#generation-and-refresh-example)

<a id="choosing-a-mode"></a>

## Choosing a cache operation

| Your goal | Configuration | What the response means |
|---|---|---|
| Delete known stale entries and wait for the deletion attempt | PURGE with the background queue off | `200 purged` or `200 no_match`; errors use 4xx/5xx |
| Schedule a large prefix or full-cache cleanup | PURGE with `cache_purge_background_queue on` | `202 accepted`; files may still exist and be served while cleanup is pending |
| Stop new requests from hitting old content, including late writes | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; new requests use a new key generation; disk cleanup follows |
| Check whether cached proxy content changed at the origin | Generation keys + `proxy_cache_refresh` + an internal profile | `200` with per-entry results and continuation; it does not prewarm the cache |

These settings combine: generation invalidation also schedules cleanup. The background-queue switch alone does not protect against late writes. Read [generation-key example](#generation-and-refresh-example) for the v4 setup, [sample configurations](#sample-configurations) for ordinary PURGE, and [HTTP results](HTTP_RESPONSES.md) before writing a client.

## Basic — inline purge method

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/images/logo.png'
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match — no matching cache entry
# 403 — client IP not in the allowed list
```

## Basic — separate purge location

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass      http://catalog_backend;
            proxy_cache     main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key "$host$uri$is_args$args";
        }

        # Capture the cache key from the URI and pass it to the zone/key form.
        # No proxy_pass or proxy_cache required in this location.
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

<a id="partial-key-purge"></a>

## Partial-key PURGE

Append `*` to the **expanded cache key** for a case-insensitive prefix match. Exact-key and Vary matching remain case-sensitive. A physical prefix may contain up to 511 bytes, excluding the trailing `*`. A longer prefix is logged once and returns `400 invalid_selector` without walking files.

With `cache_purge_key`, matching uses the logical expression and accepts at most 8096 bytes including a trailing `*`.

For a key expression ending in `$uri`, this request selects a URI prefix:

```http
PURGE /images/header*
```

The asterisk must be the last byte of the evaluated key, not merely the last byte of the URI path. Account for any query-string or variable suffix in `proxy_cache_key`. A key consisting only of `*` walks every eligible entry.

## Background queue enabled

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

## Purge all entries

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location /content {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/content/anything'
# 200 — walk completed; inspect error_log for file errors
```

## JSON responses with per-location override

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        # Default for all locations in this server.
        cache_purge_response_type json;
        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location /admin/purge {
            allow 127.0.0.1;
            deny  all;
            # Override to plain text for scripting
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

## Vary-aware purge

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

After purging a key, all gzip and Vary variants stored at different filesystem paths are also removed automatically.

<a id="generation-and-refresh-example"></a>

## Example: a product catalog

This complete example listens locally on port 8080 and forwards product requests to an application on port 8081. The application must serve `/products/42` for the Host `shop.example.com`. Replace the application address, domain and writable cache/state directories for your deployment.

`shop.example.com` is the domain sent in the HTTP Host header. It is not a directory. `/products/42` is the product URI. `catalog:` is an arbitrary, fixed cache-key prefix, not a hostname. The logical key is the prefix plus the full request URI, including its query string: `catalog:/products/42`. The REFRESH profile removes exactly `catalog:` and checks `/products/42` at the same application.

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx/catalog levels=1:2
                     keys_zone=catalog:10m;
    cache_purge_namespace catalog 1m /var/lib/nginx/cache-purge;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location /products/ {
            cache_purge_key catalog "catalog:$request_uri";
            proxy_cache catalog;
            proxy_cache_key $cache_purge_key;
            proxy_cache_lock on;
            proxy_cache_valid 200 10m;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_set_header Host shop.example.com;
            proxy_pass http://catalog_backend;
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location = /admin/refresh {
            allow 127.0.0.1;
            deny all;
            cache_purge_key catalog $http_x_cache_key;
            proxy_cache_refresh catalog $cache_purge_key;
            cache_purge_refresh_profile /internal/cache-probe catalog:;
            cache_purge_refresh_timeout 10s;
        }

        location = /internal/cache-probe {
            internal;
            proxy_cache off;
            proxy_intercept_errors off;
            proxy_pass_request_headers off;
            proxy_pass_request_body off;
            proxy_connect_timeout 1s;
            proxy_send_timeout 2s;
            proxy_read_timeout 2s;
            proxy_method HEAD;
            proxy_set_header Host shop.example.com;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since
                             $cache_purge_refresh_last_modified;
            proxy_pass http://catalog_backend$cache_purge_refresh_uri;
        }
    }
}
```

Request an existing, cacheable product. Repeat GET until `X-Cache-Status: HIT`, then check that product with REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

When the product has been changed and must be discarded immediately, send PURGE. With this configuration, `200 invalidated` makes the old generation unreachable to new requests; the next GET goes to the application.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

To check all cached product pages, select `catalog:/products/*`. For continuation, replace `<next_cursor>` with the returned value and keep the selector unchanged. Resolve HTTP errors before continuing.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```
