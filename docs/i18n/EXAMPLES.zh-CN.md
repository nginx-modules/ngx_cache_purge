[ngx_cache_purge](README.zh-CN.md)

<a id="sample-configurations"></a>

# 配置示例

前五个代码块是可供选择的完整 `nginx.conf` 示例。Vary 代码块用于在现有 `http` 配置中追加处理。最后一个示例演示 v4 的 generation 键与 REFRESH 可选流程。NGINX 在本机 8080 端口接收请求；`catalog_backend` 是位于 `127.0.0.1:8081` 的上游组名，`shop.example.com` 是其 HTTP Host。部署时请替换这些地址和可写的缓存目录。

## 本指南内容

- [选择缓存操作](#choosing-a-mode)
- [基础：内联 PURGE 方法](#basic--inline-purge-method)
- [基础：独立 PURGE location](#basic--separate-purge-location)
- [部分键 PURGE](#partial-key-purge)
- [后台清理队列](#background-queue-enabled)
- [清理全部条目](#purge-all-entries)
- [响应格式覆盖](#json-responses-with-per-location-override)
- [Vary 感知清理](#vary-aware-purge)
- [generation 键 PURGE 与 REFRESH：商品目录](#generation-and-refresh-example)

<a id="choosing-a-mode"></a>

## 选择缓存操作

| 目标 | 配置 | 响应含义 |
|---|---|---|
| 删除已知过期的条目，并等待删除操作结束 | PURGE，关闭后台队列 | `200 purged` 或 `200 no_match`；错误返回 4xx/5xx |
| 安排按前缀或全缓存清理 | PURGE，设置 `cache_purge_background_queue on` | `202 accepted`；等待清理期间文件仍可能存在并被命中 |
| 防止新请求使用旧内容，包括延迟写入 | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`；新请求使用新的键 generation，磁盘清理随后进行 |
| 检查源站内容是否变化 | generation 键 + `proxy_cache_refresh` + 内部探测配置 | `200`，附带逐条结果和续页信息；不会预热缓存 |

这些设置可以组合：generation 失效也会安排清理。仅开启后台队列无法防止延迟写入带来的旧内容。v4 的具体配置见 [generation 键示例](#generation-and-refresh-example)，普通 PURGE 见[配置示例](#sample-configurations)；编写客户端前请先阅读 [HTTP 结果](HTTP_RESPONSES.zh-CN.md)。

<a id="basic--inline-purge-method"></a>

## 基础 — 内联 PURGE 方法

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
# 200 — 条目已清理
# 200 + Cache-Purge-Code: no_match — 没有匹配的缓存条目
# 403 — 客户端 IP 不在允许列表中
```

<a id="basic--separate-purge-location"></a>

## 基础 — 独立 PURGE location

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

        # 从 URI 中取出缓存键，交给「缓存区 + 键」的形式。
        # 此 location 无需 proxy_pass 或 proxy_cache。
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

<a id="partial-key-purge"></a>

## 部分键 PURGE

在**求值后的缓存键**末尾加 `*` 可按前缀匹配，前缀比较不区分大小写；精确键与 Vary 匹配仍区分大小写。物理前缀最多 511 字节（不含末尾 `*`）。超长前缀只记录一次日志并返回 `400 invalid_selector`，不会遍历文件。

使用 `cache_purge_key` 时，匹配基于逻辑表达式，长度上限为 8096 字节（含末尾 `*`）。

若键表达式以 `$uri` 结尾，以下请求会选择 URI 前缀：

```http
PURGE /images/header*
```

星号必须是求值后整个键的最后一个字节，而不仅仅是 URI 路径的末字节。请注意 `proxy_cache_key` 中的查询参数或变量后缀。键只有 `*` 时会遍历所有符合条件的条目。

<a id="background-queue-enabled"></a>

## 启用后台队列

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

<a id="purge-all-entries"></a>

## 清理全部条目

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
# 200 — 遍历完成；如有文件错误请查看 error_log
```

<a id="json-responses-with-per-location-override"></a>

## JSON 响应及 location 级覆盖

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        # 此服务器中所有 location 的默认值。
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
            # 为脚本改用纯文本响应
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

<a id="vary-aware-purge"></a>

## Vary 感知清理

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

清理一个键后，存储在其他文件路径的 gzip 与 Vary 变体也会自动删除。

<a id="generation-and-refresh-example"></a>

## 示例：商品目录

这个完整示例在本机 8080 端口接收请求，并转发给 8081 端口的应用。应用应在 Host 为 `shop.example.com` 时提供 `/products/42`。请替换应用地址、域名以及可写的缓存和状态目录。

`shop.example.com` 是 HTTP Host 头中的域名，不是目录。`/products/42` 是商品 URI。`catalog:` 是任意的固定缓存键前缀，不是主机名。逻辑键由该前缀加上包含查询串的完整请求 URI 构成，例如 `catalog:/products/42`。REFRESH 的探测配置会精确移除 `catalog:`，向同一应用检查 `/products/42`。

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

先请求一个存在且可缓存的商品，重复 GET 直到 `X-Cache-Status: HIT`，再用 REFRESH 检查该商品：

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

商品已变更且需要立即丢弃时，发送 PURGE。在此配置下，`200 invalidated` 会让旧 generation 对新请求不可达，下一次 GET 会访问应用。

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

要检查所有已缓存的商品页面，使用 `catalog:/products/*`。需要续页时，把返回的 `<next_cursor>` 原样替换到请求中，并保持选择条件不变；先解决 HTTP 错误再继续。

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```
