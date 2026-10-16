[ngx_cache_purge](README.zh-CN.md)

<a id="directives"></a>

# 指令

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**同一 location 形式**（`from …`）在 proxy location 拦截指定 HTTP 方法，并清理匹配的缓存项。`on` 是方法 `PURGE` 的简写，`off` 禁用清理。可用 CIDR 列表限制来源，或用 `from all` 允许任意地址。在 `from` 前加上 `purge_all` 会忽略请求 URI，清空整个缓存区。

**独立 location 形式**（缓存区和键参数）只允许出现在清理 location 中，通常通过正则 location 捕获缓存键。它按名称找到缓存区，并按编译后的键表达式执行清理。该形式不能与 `proxy_cache` 或 `proxy_pass` 放在同一 location。

<a id="fastcgi_cache_purge"></a>

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

与 `proxy_cache_purge` 等效，但用于 `fastcgi_cache` / `fastcgi_cache_path` 配置的 FastCGI 缓存区。

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

与 `proxy_cache_purge` 等效，但用于 `scgi_cache` / `scgi_cache_path` 配置的 SCGI 缓存区。

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

与 `proxy_cache_purge` 等效，但用于 `uwsgi_cache` / `uwsgi_cache_path` 配置的 uWSGI 缓存区。

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

设置成功 PURGE 响应的正文格式，默认为 JSON。`Cache-Purge-Code` 与正文中的结果代码一致。模块响应包含 `Cache-Control: no-store`；错误使用 RFC 9457 定义的 `application/problem+json`。文本键按格式转义；二进制键以 Base64 无损表示，并标明编码方式。

`none` 禁用 PURGE 正文：已完成的成功操作返回 `204`，已接受的后台任务仍返回 `202`，错误保留 4xx/5xx。状态和 REFRESH 端点仍返回结构化 JSON。完整代码、字段和限制见 [HTTP 响应契约](../HTTP_RESPONSES.md)。

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

启用通配符与 `purge_all` 的延后处理。入队或合并到相同的等待／运行中任务后，返回 `202 Accepted`。不影响精确键清理。

此开关控制传统通配符／全量清理。绑定 `cache_purge_key` 后始终使用延迟物理清理并返回表示逻辑完成的 `200`，即使此开关为 off。

队列满或任务分配失败时，请求保持打开直到接纳成功，等待期间不进行目录遍历。重复的等待／运行中任务即使满容量也可接纳。如果无法分配请求所需的小型等待状态，则返回 `500`。断连 cleanup 会取消定时器并减少等待计数。

此开关和 `cache_purge_vary_aware` 使用最后一次声明的值，无效值会被拒绝。`cache_purge_legacy_status` 已废弃且被忽略。线程与生命周期见[执行模型](../ARCHITECTURE.md#execution-model)。

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

所有缓存路径合计的未完成任务上限。等待中与已分配任务均占容量。重复请求共用一次分配，重跑保留同一内存与配额；不同任务等待空闲配额。

配置后台处理或状态端点后便会创建共享区。即使取整后的共享区大小相同，容量变更仍需重启。reload 拒绝调整大小，以保留已接纳工作。增加缓存根目录也可能需要更多共享区内存并要求重启。若工作必须完成，应先排空等待中和活动任务。

声明命名空间也会创建任务队列。

共享区大小包含任务存储、路径元数据及按缓存行对齐的 worker 指标，取决于构建和缓存路径数，见[内存计算](../OPERATIONS.md#memory-sizing)。

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

按普通继承规则为已接纳的后台任务选择执行池。例如：

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX 验证命名池，并通过原生任务事件交付完成通知。模块不复制私有线程池布局，也不另建完成队列。支持线程时，自定义名称的池必须存在。不支持线程或运行于 Windows 时也接受此指令，但采用有预算限制的事件循环分片。所选池满只延迟执行，不会将 I/O 移回事件循环。独立池会增加线程栈和上下文切换开销。

<a id="cache_purge_batch_size"></a>

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

每个 worker 在限速暂停之间处理的最大任务数。进度跨遍历分片和线程完成回调保留。空队列结束当前批次。大于 64 的值会警告并限制为 64，0 会被拒绝。

此项控制任务数，而非文件数或回调时长。较小值增加任务之间的暂停，不会中断正在执行的线程遍历。调整磁盘负载时与 `cache_purge_throttle_ms` 配合使用。

<a id="cache_purge_throttle_ms"></a>

## `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

后台任务批次间的延迟。接受 `10ms`、`500ms`、`1s`，以及带引号的组合值，如 `"2s 500ms"`。无单位整数表示毫秒，因此 `10` 等同于 `10ms`。无单位会产生配置警告；显式单位可避免该警告。

空闲 worker 在该间隔的十倍之后再次检查。未完成分片及互斥锁重试使用 1 ms 定时器。延迟是最小调度间隔，不是完成期限。0 及超过平台安全定时范围的值会被拒绝。

准入、线程池压力和延迟重试使用 10 ms 轮询。

<a id="cache_purge_queue_status"></a>

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

为 GET/HEAD 提供带 `Cache-Control: no-store` 的 JSON 快照。路径必须指向已配置的缓存根；未知路径会导致配置加载失败，即使后台处理关闭也一样。请限制端点访问：

```nginx
location = /cache_purge_queue_status {
    allow 127.0.0.1;
    deny  all;
    cache_purge_queue_status /var/cache/nginx;
}
```

```json
{
  "code": "status",
  "queue": { "size": 0, "running": 0, "max_size": 1024, "wait_time": 0, "full": false },
  "cache": {
    "pending": { "total": 0, "full": 0 },
    "active": 0,
    "requests": { "overflow": 0, "deduplicated": 0, "waiting": 0 },
    "files": { "deleted": 0, "errors": 0, "bytes": 0 },
    "logical": { "invalidations": 0 },
    "cleanup": { "scan_errors": 0, "retries": 0, "exhausted": 0 },
    "responses": { "hit": 0, "miss": 0, "hit_ratio": 0.000000 }
  }
}
```

`queue` 描述所有缓存路径共用的队列：

| 字段 | 类型／单位 | 含义 |
|---|---|---|
| `size` | 整数／任务 | 等待 worker 领取的任务 |
| `running` | 整数／任务 | 已分配任务，包括等待线程池的任务 |
| `max_size` | 整数／任务 | 等待与已分配任务的总容量 |
| `wait_time` | 整数／毫秒 | 最老待处理任务的年龄，空队列为 0 |
| `full` | 布尔值 | 采样到的等待与已分配任务之和是否达到容量 |

`cache` 描述此 location 选中的缓存根：

| 字段 | 类型／单位 | 含义 |
|---|---|---|
| `pending.total` | 整数／任务 | 此缓存的待处理任务 |
| `pending.full` | 整数／任务 | 等待中的 `purge_all`，包含在 `pending.total` 内 |
| `active` | 整数／任务 | 已分配且尚未完成结果记账的任务 |
| `requests.overflow` | 整数／请求 | 进入接纳等待的请求，包括锁竞争和分配压力 |
| `requests.deduplicated` | 整数／请求 | 合并到等待或运行中工作的请求 |
| `requests.waiting` | 整数／请求 | 此缓存当前等待接纳的请求 |
| `files.deleted` | 整数／文件 | 每轮结束时上报的成功后台 unlink |
| `files.errors` | 整数／错误 | 除文件已不存在外的后台 unlink 失败 |
| `files.bytes` | 整数／字节 | 成功 unlink 文件的估算逻辑大小 |
| `responses.hit`, `responses.miss` | 整数／主请求 | 在 log phase 观察到的核心 HIT 与 MISS 判定 |
| `responses.hit_ratio` | 数值／0..1 | HIT / (HIT + MISS)，两者均未观察到时为 0 |
| `logical.invalidations` | 整数／决定数 | 已发布的世代失效，包括重复请求和 REFRESH 决定 |
| `cleanup.scan_errors` | 整数／错误数 | 后台元数据、打开、读取、关闭或分配错误 |
| `cleanup.retries` | 整数／扫描次数 | 因清理失败而安排的额外扫描 |
| `cleanup.exhausted` | 整数／任务数 | 三次重试失败后终止的清理任务 |

`pending.full` 统计全缓存操作；键为 `*` 的通配符仍是独立操作类型。`active` 包括线程池等待者。活动任务不计入 `queue.size` 和 `pending.total`，但占用接纳容量。

合并命名空间扫描时，`pending.full` 沿用首个任务的分类。REFRESH 扫描／探测不计入 PURGE 队列深度或删除错误计数。延迟重试轮转后，`wait_time` 可能只是采样到的队头等待时间，而非最长等待时间。

每个进程槽位只有一个事件循环写者，起始地址与步长均按缓存行对齐。线程通过完成事件返回结果。状态读取者不获取队列锁，也不执行原子 fetch-add，包括加 0 的操作。64 位构建使用对齐的普通 volatile 读写访问计数器。32 位构建通过有重试上限的序列检查保护 64 位读取；此时，正在写入的分片可能从某次采样中略过。字段是近似值，并非跨 worker 事务。接纳决策始终使用锁内控制状态，不使用监控数值。

累积计数器达到无符号 64 位最大值后饱和，普通 reload 后保留，重启时重置。等待量随请求 cleanup 更新。同步操作和核心缓存管理器的删除不计入后台 删除总数。字节数是文件长度估计，不代表文件系统块数或已释放磁盘空间；仍打开的读取者及并发写入都可能影响结果。已完成的部分遍历会计入，但崩溃 worker 的结果可能缺失。这些指标不是审计日志。

HIT/MISS 只统计有原生缓存判定的主请求。没有 upstream、缓存状态 0，以及 BYPASS、EXPIRED、STALE、UPDATING、REVALIDATED 都不进入该比率。普通非缓存请求不算 MISS。即使后台清理关闭，仅监控也能创建 SHM 区。32 位系统上的队列年龄适用于约 24.9 天以内的间隔；带符号时间差为负时报告 0。

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

此指令已废弃，仅为配置迁移而保留。`on` 和 `off` 都会产生警告且被忽略，请删除该指令；无效值仍会被拒绝。同步操作没有匹配项时返回 `200 no_match`，接受物理清理时返回 `202 accepted`，发布代际失效时返回 `200 invalidated`。不再支持切换 `412`/`404`。参见 [HTTP 响应契约](../HTTP_RESPONSES.md)。

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

开启后，成功的精确键清理在删除主文件之后遍历缓存目录，删除剩余的、含相同 `KEY:` 字符串的文件。这样可覆盖保存在不同文件路径、但共享同一逻辑键的 `Vary` 和 `gzip_vary` 变体。会检查键后紧接的字节必须为 `\n`，避免误匹配只有共同前缀的其他键。

默认关闭，因为每次精确清理都会增加一次完整目录遍历。存在多个 `Vary`/`gzip_vary` 表示时可启用。即使主条目已不存在，也会遍历并清理变体。通配符和 `purge_all` 已覆盖变体。精确 Vary 键区分大小写，最多 511 字节；超长键在删除任何文件前返回 `400 invalid_selector`。

世代失效无需此选项即可覆盖逻辑键的全部表示。

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

在 location 绑定前声明世代命名空间。名称为 1–32 个 ASCII 字母、数字、`_` 或 `-`；SHM 至少为八个原生页。使用缓存和临时根目录之外的独立持久状态目录。核心通过独占创建的持久标记分配重启 epoch。只要对应缓存可能仍存在，就必须保留这些小文件；保留缓存却删除状态可能导致旧 epoch 被重用。不保证断电／fsync 持久性。普通 reload 保留 epoch 和规则；修改大小、路径或移除命名空间需要完全重启。命名空间 SHM 与任务队列分别指定大小。

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

将 location 绑定到已有命名空间并提供 `$cache_purge_key`。将完整的 `proxy_cache_key`、`fastcgi_cache_key`、`scgi_cache_key` 或 `uwsgi_cache_key` 设为此变量，对应的独立 PURGE location 也必须如此。若展开后的原生键与生成键不同，PURGE 返回 `500`，不会声称完成严格失效。逻辑表达式中不要引用 `$cache_purge_key` 自身。逻辑键最大 8096 字节。若其他 location 需要传统语义，仅绑定参与的 location；没有用于取消继承绑定的 `off` 形式。

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

创建仅接受主请求的显式 REFRESH 端点，其他方法返回 `405`。需要绑定 `cache_purge_key`，原生表达式使用 `$cache_purge_key`，并配置 refresh 配置档。逻辑键选择精确键、末尾 `*` 的前缀，或用 `*` 选择此缓存路径中的整个命名空间。使用核心访问控制／认证保护此 location。REFRESH 仅支持代理缓存，PURGE 仍支持全部四种协议。

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

将一个已知表示映射到固定的内部代理 location。从逻辑键移除大小写敏感的精确前缀后，必须得到以 `/` 开头的 origin URI；查询字符串与转义保留。模块不会猜测主机，也不会在键中搜索 `://`。目标必须启用 `internal`，关闭缓存及请求头／请求体转发，使用 HEAD、非空字面值 Host 和字面值表示头。两个条件头必须分别完整使用 `$cache_purge_refresh_etag` 与 `$cache_purge_refresh_last_modified`。`$cache_purge_refresh_uri` 向 `proxy_pass` 提供映射 URI。这三个无副作用变量仅在探测中有意义。preaccess 会检查目标及内部重定向。管理员必须确保映射指向与缓存键相同的 origin 表示；原生元数据不保存原请求头，因此 Vary 条目会跳过。

在探测 location 中设置 `proxy_intercept_errors off`；继承的错误拦截会被拒绝，防止 origin 错误被改写为触发失效的成功响应。

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

限制 REFRESH 页中新扫描／探测的启动时间，包含等待 worker 扫描器的时间。到期后停止新工作并标记响应不完整。已运行的 HEAD 依赖核心 `proxy_connect_timeout`、`proxy_send_timeout` 和 `proxy_read_timeout` 完成，请显式设置它们。此值不是严格响应期限，也无法中断文件系统调用。断连或 graceful shutdown 会停止后续分派，并保留活动线程的存储直至原生完成回调。
