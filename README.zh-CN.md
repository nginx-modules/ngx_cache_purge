# ngx_cache_purge / refresh 部署说明

这份文档面向部署者，重点说明当前实现里 `purge` / `refresh` 的入口规则、动作路由、能力边界、推荐配置和常见误配。

## 核心规则

- 同一个 `location` 只能配置一个入口指令：`proxy_cache_purge` 或 `proxy_cache_refresh`。
- 对于精确匹配和通配前缀匹配（exact / partial），`proxy_cache_purge` 与 `proxy_cache_refresh` 都同时接受 `PURGE` 和 `REFRESH`。
- exact / partial 最终执行的是 `purge` 还是 `refresh`，由实际 HTTP method 决定，不由 directive 名称决定。
- 成功响应会返回 `X-Cache-Action: purge` 或 `X-Cache-Action: refresh`，用于标识本次实际动作。
- full-zone 请求不能自由互串：`PURGE` 需要 `purge_all`，`REFRESH` 需要 `refresh_all`。
- full-zone 能力不匹配时返回 `400 Bad Request`。
- 配置期规则同样严格：`purge_all` 只能用于 `proxy_cache_purge`，`refresh_all` 只能用于 `proxy_cache_refresh`。
- `proxy_cache_refresh ... purge_all ...` 是非法配置，nginx 在加载配置时就会拒绝。
- refresh 只支持 `proxy_cache`，不支持 `fastcgi_cache`、`scgi_cache`、`uwsgi_cache`。
- 参与 refresh 请求链路的 proxy 配置必须包含：

      proxy_cache_bypass  $cache_purge_refresh_bypass;
      proxy_no_cache      $cache_purge_refresh_bypass;

- refresh 能否正确工作，取决于 cache key 的最后一段是不是请求路径本身。它不要求固定写成 `$uri$is_args$args`；像 `$request_uri`、`$host$request_uri` 这样的写法也可以，因为 key 的尾部仍然是完整的 URI / request URI 部分。

## 动作路由说明

对 exact / partial 请求，动作路由遵循下面这张表：

| 配置的入口指令 | 请求 method | 实际动作 |
| --- | --- | --- |
| `proxy_cache_purge` | `PURGE` | purge |
| `proxy_cache_purge` | `REFRESH` | refresh |
| `proxy_cache_refresh` | `REFRESH` | refresh |
| `proxy_cache_refresh` | `PURGE` | purge |

要点只有一条：

> 对 exact / partial，请把 directive 名称理解成“入口类型”，不要把它误解成“最终动作已经固定”。真正决定动作的是 HTTP method。

但 full-zone 不适用这张自由互串表。full-zone 需要同时满足 method 与 capability 的匹配关系：

| full-zone 请求 | location 必须具备的能力 | 不匹配时结果 |
| --- | --- | --- |
| `PURGE /.../*` | `proxy_cache_purge ... purge_all ...` | `400 Bad Request` |
| `REFRESH /.../*` | `proxy_cache_refresh ... refresh_all ...` | `400 Bad Request` |

## 推荐配置

### 模板一：生产推荐，`/purge` 与 `/refresh` 分离

这是最稳妥的部署方式。运维、权限、监控、限流和回滚都更直观。

```nginx
http {
    proxy_cache_path  /var/cache/nginx  keys_zone=app_cache:100m;

    server {
        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        app_cache;
            proxy_cache_key    "$scheme$proxy_host$request_uri";
            proxy_cache_bypass $cache_purge_refresh_bypass;
            proxy_no_cache     $cache_purge_refresh_bypass;
        }

        location ~ /purge(/.*) {
            allow              127.0.0.1;
            deny               all;
            proxy_cache_purge  app_cache $scheme$proxy_host$1$is_args$args;
        }

        location ~ /refresh(/.*) {
            allow              127.0.0.1;
            deny               all;

            proxy_pass         http://127.0.0.1:8000;
            proxy_cache_bypass $cache_purge_refresh_bypass;
            proxy_no_cache     $cache_purge_refresh_bypass;

            proxy_cache_refresh            app_cache $scheme$proxy_host$1$is_args$args;
            cache_purge_refresh_timeout     60s;
            cache_purge_refresh_concurrency 32;
        }
    }
}
```

适用场景：

- 新部署。
- 希望把 purge 和 refresh 分开做权限控制。
- 希望接口命名与实际运维动作一一对应。

### 模板二：渐进迁移，单入口先切 method

如果你已有 `proxy_cache_purge` 入口，不想马上新增 `/refresh` endpoint，可以先让客户端切换 method。该方案只适合 exact / partial 的过渡期。

```nginx
http {
    proxy_cache_path  /var/cache/nginx  keys_zone=app_cache:100m;

    server {
        location / {
            proxy_pass         http://127.0.0.1:8000;
            proxy_cache        app_cache;
            proxy_cache_key    "$host$request_uri";
            proxy_cache_bypass $cache_purge_refresh_bypass;
            proxy_no_cache     $cache_purge_refresh_bypass;
            proxy_cache_purge  PURGE from 127.0.0.1;
        }
    }
}
```

在这套配置里：

- `PURGE /path/file` -> purge
- `REFRESH /path/file` -> refresh
- `PURGE /dir/*` -> partial purge
- `REFRESH /dir/*` -> partial refresh

注意：单入口 method 兼容不等于 full-zone 也能兼容。`REFRESH /*` 仍然需要一个显式配置了 `proxy_cache_refresh ... refresh_all ...` 的 location。

还要特别注意：单入口渐进迁移只解决“入口不想立刻拆分”的问题，不会自动豁免 refresh 的基础前提。如果请求最终按 `REFRESH` 路由执行，那么参与 refresh 请求链路的 proxy 配置仍然必须满足 bypass/no_cache 约束，cache key 仍然必须以 URI / request URI 结尾。

`cache_purge_refresh_timeout` 也只是 refresh 的总体软截止时间：超时后会停止继续派发新的校验子请求，但已经在途的子请求会自然收尾，而不是被粗暴中断。

## 能力边界

### `purge_all` 与 `refresh_all` 的边界

- `purge_all` 表示 full-zone purge 能力。
- `refresh_all` 表示 full-zone refresh 能力。
- 二者不能靠切换 method 互相替代。
- 运行时 capability mismatch 返回 `400 Bad Request`。

### refresh 的实现边界

- refresh 只支持 `proxy_cache`。
- refresh 会向 upstream 发带条件头（`If-None-Match` / `If-Modified-Since`）的校验子请求，并读取缓存文件里的 `ETag` / `Last-Modified`。
- nginx 内部仍可能把上游方法转成 `GET`，但 refresh 路径会强制只处理响应头、不读取响应体；真正的带宽收益主要来自 `304 Not Modified` 和不读 body。
- upstream 返回 `304` 时保留缓存；返回 `200` 时走正常 invalidate 路径；返回 `404` / `410` 时直接 purge；其它 HTTP 状态默认保留缓存，并额外记入状态码统计；只有内部/传输失败才计入 `errors`。

### cache key 边界

可工作的典型 key：

- `$uri`
- `$uri$is_args$args`
- `$host$request_uri`
- `$scheme$proxy_host$request_uri`

不可靠的典型 key：

- `$uri$host`
- `$request_uri$cookie_user`
- `$arg_x$uri$host`

判断标准不是“key 里有没有出现 URI”，而是“key 的最后一段是不是完整的请求路径部分”。只要 key 的尾部仍然是 URI / request URI，refresh 就能从这里反推出上游请求；如果 URI 只出现在中间，末尾又拼了别的维度，这个反推过程就不可靠。

## 常见错误配置

### 错误一：同一个 `location` 里同时放两个入口指令

```nginx
location /control/ {
    proxy_cache_purge   PURGE from 127.0.0.1;
    proxy_cache_refresh REFRESH from 127.0.0.1;
}
```

为什么错：同一个 `location` 只能有一个入口指令。这个组合会在配置期被拒绝。

### 错误二：把 `purge_all` 写到 `proxy_cache_refresh` 上

```nginx
location /refresh-all/ {
    proxy_cache_refresh REFRESH purge_all from 127.0.0.1;
}
```

为什么错：`purge_all` 属于 `proxy_cache_purge`，这里必须写 `refresh_all`。

### 错误三：把 full-zone `PURGE` 发到只有 `refresh_all` 的入口

```nginx
location /refresh-all/ {
    allow               127.0.0.1;
    deny                all;
    proxy_pass          http://127.0.0.1:8000;
    proxy_cache_bypass  $cache_purge_refresh_bypass;
    proxy_no_cache      $cache_purge_refresh_bypass;
    proxy_cache_refresh REFRESH refresh_all from 127.0.0.1;
}
```

如果这里是 `REFRESH ... refresh_all ...` 能力，那么客户端发 full-zone `PURGE` 仍会因为 capability mismatch 返回 `400 Bad Request`。

### 错误四：refresh 链路缺少 bypass / no_cache

```nginx
location / {
    proxy_pass      http://127.0.0.1:8000;
    proxy_cache     app_cache;
    proxy_cache_key "$host$request_uri";
}
```

为什么错：如果 refresh 子请求最终走到这样的 proxy location，而没有：

```nginx
proxy_cache_bypass $cache_purge_refresh_bypass;
proxy_no_cache     $cache_purge_refresh_bypass;
```

那么 refresh 可能误读本地缓存，或者把校验请求重新写回缓存。

### 错误五：把 refresh 用到非 proxy cache

```nginx
location /fcgi-refresh/ {
    fastcgi_cache_purge REFRESH from 127.0.0.1;
}
```

为什么错：当前 refresh 不支持 `fastcgi` / `scgi` / `uwsgi`。

### 错误六：cache key 的末尾不是 URI / request URI

```nginx
location / {
    proxy_pass         http://127.0.0.1:8000;
    proxy_cache        app_cache;
    proxy_cache_key    "$arg_x$uri$host";
    proxy_cache_purge  PURGE from 127.0.0.1;
}
```

为什么错：refresh 需要从缓存 key 的最后一段反推出上游请求路径。只要 URI / request URI 不在 key 的末尾，这个反推过程就不可靠。

## 生产部署建议

- 新部署优先使用“双入口”模型：`/purge/...` 和 `/refresh/...` 分离。
- 只有在迁移期才建议使用“单入口 + method 切换”。
- refresh endpoint 建议做单独的访问控制、限流、审计和监控。
- 对大规模 refresh，结合业务体量调 `cache_purge_refresh_concurrency` 和 `cache_purge_refresh_timeout`。
- 生产前先核对 `proxy_cache_key` 是否以 URI / request URI 结尾；这是功能正确性的前提，不只是文档建议。
- 检查 refresh 子请求实际会经过哪些 proxy location，确保这些 location 都正确配置了 `proxy_cache_bypass` 与 `proxy_no_cache`。
- 把 `X-Cache-Action` 纳入日志或审计字段，这样能快速确认一次请求最终执行的是 purge 还是 refresh。

## 响应与排错提示

- refresh 成功时返回 `200 OK`；refresh 目前只支持 JSON 或 text 两种正文格式：当 `cache_purge_response_type=json` 时返回 JSON，例如 `{"status":"refresh",...,"status_counts":{"200":190,"301":1,"304":15375}}`；其余取值都会回退到 text。默认 text 模式下，正文类似 `Refresh: total=<N> kept=<K> purged=<P> errors=<E> statuses={200:<N>,304:<N>,...}`。
- 其中统计口径是：`kept` 表示本次 refresh 的最终动作是保留缓存（例如上游返回 `304`、`301`、`403`、`500`，或者竞态下保留缓存）；`purged` 表示最终动作是清理缓存（当前主要对应上游返回 `200`、`404`、`410` 且 invalidate 成功）；`errors` 只表示真正需要排查的失败，例如子请求创建失败、超时、传输失败、内部 invalidate/helper 失败。
- `statuses={...}` 表示本次 refresh 实际观察到的 upstream HTTP 状态码统计；只显示本次请求里真正出现过的状态码，不做穷举。
- purge / refresh 成功时都带 `X-Cache-Action` 响应头。
- full-zone 能力错配时返回 `400 Bad Request`，优先检查是不是把 `PURGE` 发到了 `refresh_all` location，或者把 `REFRESH` 发到了 `purge_all` location。
- refresh 当前对上游状态码的策略是保守的：`304` 保留，`200` 走正常 invalidate 路径，`404` / `410` 直接 purge，其它 HTTP 状态默认保留并写入 `statuses={...}`；只有内部/传输失败才计入 `errors`。
- 每次 refresh 结束时，模块还会在 `error_log notice` 写一条汇总日志，例如 `cache refresh summary uri="/path/*" total=<N> kept=<K> purged=<P> errors=<E> timed_out=<0|1> statuses={200:190,301:1,304:15375}`；逐条条目的明细仍然只在 debug 日志中可见。
- 如果 refresh 结果异常，优先检查三件事：是否是 `proxy_cache`、refresh 链路上是否配置 bypass/no_cache、cache key 是否以 URI/request URI 结尾。
