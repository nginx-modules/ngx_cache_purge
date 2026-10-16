<a id="ngx_cache_purge"></a>

# ngx_cache_purge

用于删除 `FastCGI`、`proxy`、`SCGI` 和 `uWSGI` 缓存条目的 NGINX 模块。v4 的键世代机制可在文件删除前，阻止新请求使用旧条目。REFRESH 则向源站检查代理缓存的内容是否已变化。这两项功能都需要显式配置。原有 PURGE 语法仍可使用，但 HTTP 响应已有变化，详见[迁移说明](README.zh-CN.md#migration)。

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

<a id="choosing-a-mode"></a>

## 根据需求选择处理方式

| 目标 | 配置 | 响应含义 |
|---|---|---|
| 删除已知过期的条目，并等待删除操作结束 | PURGE，关闭后台队列 | `200 purged` 或 `200 no_match`；错误返回 4xx/5xx |
| 安排按前缀或全缓存清理 | PURGE，设置 `cache_purge_background_queue on` | `202 accepted`；等待清理期间仍可能返回旧条目 |
| 防止新请求使用旧内容，包括延迟写入的文件 | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`；新请求使用新世代的键，文件随后清理 |
| 检查源站内容是否变化 | 键世代 + `proxy_cache_refresh` + 内部配置位置 | `200`，附带逐条结果和续页信息；不会预热缓存 |

这些机制可以组合使用：切换世代也会安排文件清理。仅开启后台队列无法防止延迟写入带来的旧内容。参见 [v4 示例](README.zh-CN.md#generation-and-refresh-example)、[普通 PURGE 示例](EXAMPLES.zh-CN.md#sample-configurations)及 [HTTP 响应说明](../HTTP_RESPONSES.md)（英文）。

<a id="compatibility"></a>

## 兼容性

生产环境请使用仍受维护的 NGINX 版本。主要面向 POSIX/UNIX；Windows 支持为实验性。线程可选。 [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="installation"></a>

## 安装

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

动态模块：

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

请使用与服务器上安装的 NGINX 版本完全一致的源代码。`--with-compat` 并不保证任意 NGINX 版本或 C 库之间的兼容性。详见[动态模块与发布归档](../BINARY_MODULES.md)（英文）。

<a id="generation-and-refresh-example"></a>

## 示例：商品目录

此完整示例在本机 8080 端口接收请求，并转发给 8081 端口的应用。应用应在 Host 为 `shop.example.com` 时提供 `/products/42`。请替换应用地址、域名及可写的缓存和状态目录。

`shop.example.com` 是 HTTP Host 域名，不是目录。`/products/42` 是商品 URI。`catalog:` 是固定的缓存键前缀，不是主机名。前缀加上包含查询参数的完整 URI 构成逻辑键，例如 `catalog:/products/42`。REFRESH 配置移除 `catalog:`，向同一应用检查 `/products/42`。

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

先 GET 一个存在且可缓存的商品，重复至 `X-Cache-Status: HIT`，再发送 REFRESH：

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

商品已变更时，发送 PURGE。这里 `200 invalidated` 表示新请求不能命中旧世代；下次 GET 会访问应用。

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

检查所有商品时使用 `catalog:/products/*`。若 `complete: false`，保持选择条件不变，将返回值替换 `<next_cursor>` 后继续。先解决 HTTP 错误，再重试。

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## 部分键清理

在**求值后的缓存键**末尾添加 `*`，按前缀匹配且不区分大小写。精确键和 Vary 匹配仍区分大小写。物理清理前缀最多 511 字节，不含末尾的 `*`。超长前缀只记录一次日志，并返回 `400 invalid_selector`，不遍历文件。

启用 `cache_purge_key` 时匹配逻辑表达式，长度上限为 8096 字节（含末尾 `*`）。

若键表达式以 `$uri` 结尾，以下请求可选择 URI 前缀：

```http
PURGE /images/header*
```

星号必须是求值后整个键的最后一个字节，而不仅仅是 URI 路径的末字节。请考虑 `proxy_cache_key` 的查询参数或变量后缀。键只有 `*` 时会遍历所有符合条件的条目。

<a id="generation-invalidation"></a>

## 世代失效

使用键世代可让 PURGE 完成后的新请求无法命中旧内容，包括稍后才写入的旧响应。文件会另行清理。命名空间共享失效规则；重启时请保留其状态目录。

<a id="conditional-refresh"></a>

## 向源站检查缓存内容（REFRESH）

REFRESH 使用缓存的 ETag 和 Last-Modified，向同一源站表示发送 HEAD 检查。已知商品页面过期时，直接使用 PURGE。REFRESH 不下载新内容，不预热空缓存，也不延长 TTL。 [HTTP / REFRESH](../REFRESH.md).

| origin 原始结果 | 操作／结果 |
|---|---|
| `304` | 保留；`kept` |
| `200`、`404`、`410` | 条件式世代失效并延迟清理；`invalidated` |
| 其他状态 | 保留；`upstream_status` |
| 无 origin 状态／传输故障 | 保留；`transport_error` |
| Vary 元数据 | 不发送 HEAD；`skipped_vary` |
| 无效映射或配置档 | 保留；`skipped_profile` |
| 世代已被替换 | 不再次失效；`raced_replaced` |
| 内部失效／分配失败 | 保留；`invalidation_error` |
| 页停止前未分派／未完成 | `undispatched`；游标不会越过该项 |

<a id="migration"></a>

## 迁移

共享内存布局变更需要完整重启。方法／ACL 语法和协议处理器保持可用。v4 默认使用 JSON，响应头和正文提供一致的结果代码，缺失条目返回 `200 no_match`，`cache_purge_legacy_status` 仅产生警告。请依据 [HTTP 响应契约](../HTTP_RESPONSES.md) 更新客户端。队列容量包含已分配任务；过载请求等待接纳。运行中的重复任务可以预约再次遍历。

<a id="security"></a>

## 安全

- 通过 `allow` / `deny` 或前置认证限制 PURGE 端点，切勿公开暴露。
- 根据流量配置 `cache_purge_queue_size`，限制请求洪峰中的共享内存消耗。
- 可为清理 location 配合 `limit_req`，独立于普通流量限制清理请求速率。

<a id="documentation"></a>

## 文档

- [指令参考](DIRECTIVES.zh-CN.md)
- [更多配置示例](EXAMPLES.zh-CN.md)
- [运行与故障排查（英文）](../OPERATIONS.md)
- [HTTP 与 REFRESH 限制（英文）](../REFRESH.md)
- [迁移到 v4（英文）](../MIGRATION.md)
- [实现架构（英文）](../ARCHITECTURE.md)
- [开发与测试（英文）](../../t/TESTING.md)

<a id="see-also"></a>

## 另请参阅

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)

<a id="license"></a>

## 许可证

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denis Denisov

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```
