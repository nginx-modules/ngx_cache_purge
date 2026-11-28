

<a id="contents"></a>

## 目录

- [功能](#features)
- [兼容性](#compatibility)
- [发行版软件包](#distribution-packages)
- [安装](#installation)
- [使用方法](#usage)
  - [部分键清理](#partial-key-purge)
  - [按 generation 失效](#generation-invalidation)
  - **[REFRESH](#conditional-refresh)** — 向源站检查缓存内容
- [配置示例](#localized-examples)
- [安全](#security)
- [迁移](#migration)
- [文档](#documentation)
- [另请参阅](#see-also)
- [赞助方](#sponsors)
- [许可证](#license)

<a id="features"></a>

## 功能

- **同一 location 内清理** — 使用专用 HTTP 方法（`PURGE`）和 IP 访问控制
- **独立清理 location** — 通过 `proxy_cache_purge zone key` 指定正则捕获的键，无需 `proxy_pass`
- **通配符／部分匹配** — 末尾的 `*` 触发缓存目录遍历并删除匹配条目
- **`purge_all`** — 用一次请求删除缓存区内的全部条目
- **后台队列** — 通过有界的事件循环分片或可选的 NGINX 线程池延后遍历；批量大小和暂停时间可配置
- **Vary 感知清理** — 精确键清理后，删除共享同一键的 gzip、Vary 等文件变体
- **响应格式** — `json`（默认）、`html`、`xml`、`text`、`none`
- **命名线程池** — 可继承的线程池选择；不支持线程的构建及 Windows 使用增量回退路径
- **运行中任务去重** — 遍历期间收到重复请求时，索引中只保留一个任务，并最多安排一次重放
- **状态监控** — 按缓存提供队列、删除和 HIT/MISS 指标，读取时不获取共享队列互斥锁
- **generation 键** — 对精确键、前缀和命名空间执行失效，无需在 PURGE 请求中进行磁盘 I/O；迟到的旧写入仍不可访问
- **[REFRESH](#conditional-refresh)** — 向源站检查缓存内容

<a id="compatibility"></a>

## 兼容性

生产环境请使用仍受维护的 NGINX 版本。主要面向 POSIX/UNIX；Windows 支持为实验性。线程可选。参见[已测试的版本与平台](../../t/TESTING.md#compatibility-matrix)。

<a id="distribution-packages"></a>

## 发行版软件包

部分发行版和 ports 项目提供此分支的安装包，但不一定是 v4。安装前请核对软件包版本及 NGINX ABI 兼容性。详情参见[软件包及其来源](DISTRIBUTIONS.zh-CN.md)。

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

请使用与服务器上安装的 NGINX 版本完全一致的源代码。`--with-compat` 并不保证任意 NGINX 版本或 C 库之间的兼容性。详见[动态模块与发布归档](BINARY_MODULES.zh-CN.md)。

<a id="usage"></a>

## 使用方法

[→ 配置示例](#localized-examples)

<a id="partial-key-purge"></a>

### 部分键清理

在**求值后的缓存键**末尾加 `*` 可按前缀匹配；前缀比较不区分大小写。精确键和 Vary 匹配仍区分大小写。物理清理前缀最多 511 字节，不含末尾的 `*`。超长前缀只记录一次日志，并返回 `400 invalid_selector`，不遍历文件。

启用 `cache_purge_key` 时匹配逻辑表达式，长度上限为 8096 字节（含末尾 `*`）。

若键表达式以 `$uri` 结尾，以下请求可选择 URI 前缀：

```http
PURGE /images/header*
```

星号必须是求值后整个键的最后一个字节，而不仅仅是 URI 路径的末字节。请考虑 `proxy_cache_key` 的查询参数或变量后缀。键只有 `*` 时会遍历所有符合条件的条目。

<a id="generation-invalidation"></a>

### 按 generation 失效

每个逻辑键的 `generation` 由命名空间的 `serial` 推导，而不是每个键各自加一：`serial` 在每次失效时递增，匹配的规则取该值，因此先失效 A、再失效 B 时，B 的 generation 可能从 1 跳到 3。单调性只在同一个 `epoch` 内成立。generation 变化后，PURGE 完成后的新请求就无法命中旧内容，包括稍后才写入的旧响应。物理删除随后由后台清理完成。命名空间共享同一套失效规则；其状态目录必须与缓存一起保留。

优雅关闭时会保存命名空间状态，因此计划内的重启会复用 epoch 与失效规则，而不是让缓存重新变冷。若上次未走到关闭钩子，会留下运行标记，下次启动将轮换 epoch，确保已失效的条目仍然不可达。

<a id="conditional-refresh"></a>

### 向源站检查缓存内容（REFRESH）

REFRESH 使用缓存的 ETag 和 Last-Modified，向同一源站发送条件式 HEAD 请求。已知商品页面过期时，直接使用 PURGE。REFRESH 不下载新内容，不预热空缓存，也不延长 TTL。详见 [HTTP / REFRESH 说明](REFRESH.zh-CN.md)。

| 源站返回 | 操作／结果 |
|---|---|
| `304` | 保留；`kept` |
| `200`、`404`、`410` | 递增 generation 并延迟清理；`invalidated` |
| 其他状态 | 保留；`upstream_status` |
| 无 origin 状态／传输故障 | 保留；`transport_error` |
| Vary 元数据 | 不发送 HEAD；`skipped_vary` |
| 无效的映射或探测配置 | 保留；`skipped_profile` |
| generation 已被替换 | 不再次失效；`raced_replaced` |
| 内部失效／分配失败 | 保留；`invalidation_error` |
| 本页停止前仍未分派或未完成的工作 | `undispatched`；游标不会越过该项 |

<a id="localized-examples"></a>

## 配置示例

完整示例包含使用 REFRESH 的商品目录以及常规 PURGE 配置，见[配置示例](EXAMPLES.zh-CN.md)。


<a id="choosing-a-mode"></a>

### 根据需求选择处理方式

| 目标 | 配置 | 响应含义 |
|---|---|---|
| 删除已知过期的条目，并等待删除操作结束 | PURGE，关闭后台队列 | `200 purged` 或 `200 no_match`；错误返回 4xx/5xx |
| 安排按前缀或全缓存清理 | PURGE，设置 `cache_purge_background_queue on` | `202 accepted`；等待清理期间仍可能返回旧条目 |
| 防止新请求使用旧内容，包括延迟写入的文件 | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`；新请求使用递增后 generation 的键，文件随后清理 |
| 检查源站内容是否变化 | generation 键 + `proxy_cache_refresh` + 内部探测 location | `200`，附带逐条结果和续页信息；不会预热缓存 |

这些机制可以组合使用：递增 generation 也会安排文件清理。仅开启后台队列无法防止延迟写入带来的旧内容。参见 [v4 示例](#localized-examples)、[普通 PURGE 示例](#localized-examples) 以及 [HTTP 响应说明](HTTP_RESPONSES.zh-CN.md)。

<details>
<summary>配置示例 — 7 个示例</summary>

<a id="generation-and-refresh-example"></a>

### 示例：商品目录

此完整示例在本机 8080 端口接收请求，并转发给 8081 端口的应用。应用应在 Host 为 `shop.example.com` 时提供 `/products/42`。请替换应用地址、域名及可写的缓存和状态目录。

`shop.example.com` 是 HTTP Host 域名，不是目录。`/products/42` 是商品 URI。`catalog:` 是固定的缓存键前缀，不是主机名。前缀加上包含查询参数的完整 URI 构成逻辑键，例如 `catalog:/products/42`。REFRESH 配置会剥离 `catalog:` 前缀，向同一应用探测 `/products/42`。

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

商品已变更时，发送 PURGE。这里 `200 invalidated` 表示新请求不能命中旧 generation；下次 GET 会访问应用。

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

检查所有商品时使用 `catalog:/products/*`。若 `complete: false`，保持选择条件不变，把返回的 `next_cursor` 原样用于下一次请求。先解决 HTTP 错误，再重试。

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="sample-configurations"></a>

### 配置示例

前五个代码块分别是完整的 `nginx.conf` 配置示例，请选择其中一个。最后一个代码块用于在现有 `http` 配置中启用 Vary 处理。NGINX 在本机的 8080 端口接收请求；`catalog_backend` 是 upstream 组名，应用地址为 `127.0.0.1:8081`。`shop.example.com` 是发送给应用的 HTTP Host。部署时请替换这些地址，并使用 NGINX 可写的缓存目录。

<a id="basic--inline-purge-method"></a>

#### 基础 — 同一 location 的 PURGE 方法

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

#### 基础 — 独立清理 location

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

        # 从 URI 中取出缓存键，以「缓存区 + 键」的形式传入。
        # 此 location 无需 proxy_pass 或 proxy_cache。
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

<a id="background-queue-enabled"></a>

#### 启用后台队列

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

#### 清理全部条目

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
# 200 — 扫描完成；如有文件错误请查看 error_log
```

<a id="json-responses-with-per-location-override"></a>

#### JSON 响应及 location 级覆盖

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

#### Vary 感知清理

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

清理一个键后，存储在其他文件路径的 gzip 和 Vary 变体也会自动删除。

</details>

<a id="security"></a>

## 安全

- 通过 `allow` / `deny` 或前置认证限制 PURGE 端点，切勿公开暴露。
- 根据流量配置 `cache_purge_queue_size`，限制请求洪峰中的共享内存消耗。
- 可为清理 location 配合 `limit_req`，独立于普通流量限制清理请求速率。

<a id="migration"></a>

## 迁移

共享内存布局变更需要完整重启。方法／ACL 语法和各协议处理器保持可用。v4 默认使用 JSON，响应头和正文提供一致的结果代码，缺失条目返回 `200 no_match`，`cache_purge_legacy_status` 仅产生警告。请依据 [HTTP 响应契约](../HTTP_RESPONSES.md) 更新客户端。队列容量包含已分配的任务；过载时请求会等待准入。运行中的重复任务可以安排一次重放。

<a id="documentation"></a>

## 文档

- [指令参考](DIRECTIVES.zh-CN.md)
- [更多配置示例](#localized-examples)
- [运行与故障排查](OPERATIONS.zh-CN.md)
- [HTTP 与 REFRESH 限制](REFRESH.zh-CN.md)
- [迁移到 v4](MIGRATION.zh-CN.md)
- [实现架构](ARCHITECTURE.zh-CN.md)
- [开发与测试（英文）](../../t/TESTING.md)

<a id="see-also"></a>

## 另请参阅

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)

<a id="sponsors"></a>

## 赞助方

感谢以下组织免费提供工具和服务，支持 `ngx_cache_purge` 这一开源项目的开发与维护：
- [PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - 静态代码分析工具，适用于企业级软件（C、C++、C#、Go 和 Java）及 Web（JS 和 TS）开发。

<a id="license"></a>

## 许可证

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denys Denysov

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
