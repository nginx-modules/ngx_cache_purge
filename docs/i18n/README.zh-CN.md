<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

用于清除 `FastCGI`、`proxy`、`SCGI` 和 `uWSGI` 缓存的 NGINX 模块。4.0 保留原有接口，新增需显式启用的世代失效机制：先使缓存不可访问，再延迟删除文件；并为代理缓存提供有界的条件式 REFRESH。

---

<a id="contents"></a>

## 目录

- [功能](#features)
- [兼容性](#compatibility)
- [安装](#installation)
- [执行模型](#execution-model)
- [指令](#directives)
- [部分键清理](#partial-key-purge)
- [世代失效](#generation-invalidation)
- [条件式 REFRESH](#conditional-refresh)
- [配置示例](#sample-configurations)
- [性能调优](#performance-tuning)
- [监控与调试](#monitoring-and-debugging)
- [故障排查](#troubleshooting)
- [测试](#testing)
- [迁移](#migration)
- [临时目录](#temporary-directories)
- [安全](#security)
- [许可证](#license)

---

<a id="features"></a>

## 功能

- **同一 location 内清理** — 使用专用 HTTP 方法（`PURGE`）和 IP 访问控制
- **独立清理 location** — 通过 `proxy_cache_purge zone key` 指定正则捕获的键，无需 `proxy_pass`
- **通配符／部分匹配** — 末尾的 `*` 触发缓存目录遍历并删除匹配条目
- **`purge_all`** — 用一次请求删除缓存区内的全部条目
- **后台队列** — 通过有预算限制的事件循环分片或可选的 NGINX 线程池延后遍历；批量大小和暂停时间可配置
- **Vary 感知清理** — 精确键清理后，删除共享同一键的 gzip、Vary 等文件变体
- **响应格式** — `json`（默认）、`html`、`xml`、`text`、`none`
- **命名线程池** — 可继承的线程池选择；不支持线程的构建及 Windows 使用增量回退路径
- **运行中任务去重** — 遍历期间收到重复请求时，索引中保留一个任务，最多预约一次后续重跑
- **状态监控** — 按缓存提供队列、删除和 HIT/MISS 指标，读取时不获取共享队列互斥锁
- **世代键** — 对精确键、前缀和命名空间执行失效，无需在 PURGE 请求中进行磁盘 I/O；迟到的旧写入仍不可访问
- **条件式 REFRESH** — 显式 HEAD 配置、共享探测额度及每页 32 项的续查机制

---

<a id="compatibility"></a>

## 兼容性

下表列出 CI 中配置的目标。某个作业成功，仅验证对应源码与构建配置，并不覆盖同一系列的所有版本。

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Linux 作业与本地执行使用相同的 Docker 镜像和测试入口。原生 macOS 与 Windows 作业验证各自的文件系统和进程 API。Windows 使用 MSVC 构建，并通过 `nginx.exe` 检查 HTTP、路径及 reload。POSIX 进程／信号测试夹具运行于 Linux 和 macOS；Windows 不运行这些夹具，也不使用可选的 NGINX 线程池执行路径。

未启用 `--with-threads` 时使用增量遍历。在 NGINX 1.7.12 之前的 epoll 构建中，也使用此模式以避开核心通知竞态。BSD 没有原生 CI。常规运行检查当前 stable/mainline 和 API 边界；扩展运行增加历史版本及 Valgrind。仅修改文档时跳过构建。运行计划和命令见[测试说明](../../t/TESTING.md)。

代码按配置、请求处理、队列、遍历、失效、REFRESH 和响应划分。通用路径函数位于 `ngx_cache_purge.h`，平台操作位于 POSIX/Win32 头文件。`generation.*` 已更名为 `invalidation.*`；内部的 generation 仍表示键的版本。参见[模块结构](../ARCHITECTURE.md)。

模块遵循 NGINX 的 C89 兼容风格，使用核心 event、slab、rbtree 和 atomic API，不要求 C11 原子操作或更新的文件系统 API。严格 GNU89 构建检查声明顺序与警告；实际验证的编译器和平台随测试结果记录。

---

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
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="execution-model"></a>

## 执行模型

一个**任务**是一次通配符或 `purge_all` 操作。一个**批次**是 worker 在限速暂停之前处理的任务数。一个**分片**是增量目录遍历中作业量有界的一步；一个任务可能需要许多分片。

启用世代键后，每个缓存路径与命名空间用一个物理扫描任务合并已接受的失效请求。REFRESH 页是独立 HTTP 操作，不属于 PURGE 队列任务。

| 请求和配置 | 文件遍历 | HTTP 完成时机 |
|---|---|---|
| 传统精确键，启用或未启用 Vary 匹配 | 请求 worker；Vary 增加一次目录遍历 | 操作结束后 |
| 通配符或 `purge_all`，后台队列关闭 | 请求 worker 同步遍历 | 遍历结束后 |
| 后台任务，使用所选线程池 | 每个 worker 同时最多一个线程池任务 | 接纳后返回 `202` |
| 后台任务，无可用线程池 | worker 事件循环增量分片 | 入队后返回 `202` |
| 队列容量或分配空间耗尽 | 请求等待接纳，不启动请求专属遍历 | 接纳后返回 `202` |
| 世代 PURGE，任意键形式 | 延迟扫描命名空间；请求内不打开或删除缓存文件 | 发布失效状态并入队后返回 `200 invalidated` |
| REFRESH 端点 | 有界扫描及最多 32 次串行 HEAD 探测 | 返回包含完成状态及续查字段的 `200` JSON |

未使用世代键的行描述传统行为。因容量等待的世代请求在获准后返回 `200`，不是 `202`。

一个增量分片最多执行 64 次目录迭代，或在观察到耗时达到 5 ms 时停止。跳过的条目与目录结束也计入预算。但该预算无法限制单次文件系统调用的耗时。

要将后台文件操作移到线程中，请使用 `--with-threads` 构建，并在 `http` 外部的 **main** 上下文定义核心线程池：

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` 可在 `http`、`server` 或 `location` 中选择命名线程池，默认值为 `default`。引用 `default` 却没有显式声明时，NGINX 会以其标准参数创建该池。其他名称必须通过核心 `thread_pool` 声明。选择规则会继承；合并任务的执行池由第一个被接纳的请求决定。每个 worker 有自己的核心线程池。池满时保留任务的接纳配额，并在 10 ms 后重试投递，不会因此启动同步遍历。未编译线程支持、Windows、单进程模式，或 reload 后旧任务的池已被移除时，使用增量游标执行。

REFRESH 在所选线程池中为每个 worker 最多增加一个扫描任务。原生 HTTP 子请求在事件循环中运行，I/O 线程不访问它们的状态。

队列和索引的结构修改使用 slab 互斥锁。接纳与后台回调使用 trylock，忙时通过定时器重试。等待中**和运行中**的任务都保留在每个缓存路径的核心字符串 rbtree 中，通过完整键比较解决哈希冲突。该路径有 `Q` 个未完成任务时，索引操作为 `O(log Q)`；路径查找与已注册路径数量呈线性关系。等待与活动任务由原生 `ngx_queue_t` 链表管理。运行中操作的第一个重复请求预约一次重跑，后续重复请求合并到该预约。当前遍历结束后，重跑进入等待队列尾部，因此较晚的 PURGE 不会被默默丢弃。持续到来的新 PURGE 可能引发更多轮遍历。只有最终完成后才释放容量配额。文件操作与响应格式化在锁外执行。

rbtree 仅用于物理清理任务去重。世代规则存储在独立 SHM 区域的有界字节 trie 中。命名空间扫描与传统键任务具有不同索引标识；REFRESH 的失效决定也进入同一清理流程。

队列已满或任务分配失败时，HTTP 请求通过 10 ms 重试定时器等待接纳，并使用核心的断连检测。请求及 cleanup 处理器保留在所属 worker，SHM 不保存请求指针。不会关闭 HTTP/2 或 HTTP/3 整条连接的读取。队列压力不会产生 429/503；已读入的短 PURGE 也不保证触发 TCP Zero Window。等待请求仍消耗内存与连接，请根据部署的连接预算使用核心超时和接纳策略。

已接纳任务在普通 reload 后保留键、池名称和临时路径保护设置。较新的重复请求会更新等待任务或下一次重跑的保护设置。禁止新的后台接纳不会丢弃已接纳工作。优雅退出的 worker 完成当前一轮后不再领取新任务。快速退出会将未完成的增量任务交回等待队列，不会在 exit hook 中完成整个文件遍历。已投递线程任务的生命周期由核心线程池销毁流程负责。

已分配任务由 PID 和槽位代次标识。worker 每秒执行一轮恢复，最多检查 64 条活动记录，并将所有者已退出的任务重新入队。这可恢复失去所有者的任务分配，但不是事务日志。在共享链表或 slab 修改期间崩溃，仍可能破坏状态并需要重启。崩溃前刚执行的删除可能不进入指标。待处理任务和重跑需要存活的 worker；若要求完成，应在停止前排空。队列不持久化。

内部队列布局不是升级 ABI。更换为不同布局的构建时，需要完整重启；必要时先完成已接纳工作。同一构建的普通 reload 会复用共享区。

删除采用尽力而为语义。`202` 表示已接纳，不代表删除完成；遍历结束也不是缓存的原子快照。并发请求可能在遍历期间或之后创建缓存，文件错误会写入日志。通配符与全缓存遍历会 unlink 文件，但不会同步更新每个核心缓存节点。精确键和 Vary 操作会使匹配节点失效。

后台扫描失败后分别等待 100 ms、1 秒和 10 秒，最多重试三次。延迟任务保留去重记录和队列额度，但不阻止其他任务执行。新 PURGE 会重置重试窗口。耗尽次数后记录错误并移除任务；检查 `cleanup.exhausted`，修复存储问题后重试。物理清理失败不会撤销世代失效。文件已消失视为正常竞争，格式无效的文件会跳过，不会无限重试。

---

<a id="directives"></a>

## 指令

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

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

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

与 `proxy_cache_purge` 等效，但用于 `fastcgi_cache` / `fastcgi_cache_path` 配置的 FastCGI 缓存区。

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

与 `proxy_cache_purge` 等效，但用于 `scgi_cache` / `scgi_cache_path` 配置的 SCGI 缓存区。

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

与 `proxy_cache_purge` 等效，但用于 `uwsgi_cache` / `uwsgi_cache_path` 配置的 uWSGI 缓存区。

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

设置成功 PURGE 响应的正文格式，默认为 JSON。`Cache-Purge-Code` 与正文中的结果代码一致。模块响应包含 `Cache-Control: no-store`；错误使用 RFC 9457 定义的 `application/problem+json`。文本键按格式转义；二进制键以 Base64 无损表示，并标明编码方式。

`none` 禁用 PURGE 正文：已完成的成功操作返回 `204`，已接受的后台任务仍返回 `202`，错误保留 4xx/5xx。状态和 REFRESH 端点仍返回结构化 JSON。完整代码、字段和限制见 [HTTP 响应契约](../HTTP_RESPONSES.md)。

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

启用通配符与 `purge_all` 的延后处理。入队或合并到相同的等待／运行中任务后，返回 `202 Accepted`。不影响精确键清理。

此开关控制传统通配符／全量清理。绑定 `cache_purge_key` 后始终使用延迟物理清理并返回表示逻辑完成的 `200`，即使此开关为 off。

队列满或任务分配失败时，请求保持打开直到接纳成功，等待期间不进行目录遍历。重复的等待／运行中任务即使满容量也可接纳。如果无法分配请求所需的小型等待状态，则返回 `500`。断连 cleanup 会取消定时器并减少等待计数。

此开关和 `cache_purge_vary_aware` 使用最后一次声明的值，无效值会被拒绝。`cache_purge_legacy_status` 已废弃且被忽略。线程与生命周期见[执行模型](#execution-model)。

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

所有缓存路径合计的未完成任务上限。等待中与已分配任务均占容量。重复请求共用一次分配，重跑保留同一内存与配额；不同任务等待空闲配额。

配置后台处理或状态端点后便会创建共享区。即使取整后的共享区大小相同，容量变更仍需重启。reload 拒绝调整大小，以保留已接纳工作。增加缓存根目录也可能需要更多共享区内存并要求重启。若工作必须完成，应先排空等待中和活动任务。

声明命名空间也会创建任务队列。

共享区大小包含任务存储、路径元数据及按缓存行对齐的 worker 指标，取决于构建和缓存路径数，见[内存计算](#memory-sizing)。

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

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

### `cache_purge_batch_size`

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

### `cache_purge_throttle_ms`

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

### `cache_purge_queue_status`

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

累积计数器达到无符号 64 位最大值后饱和，普通 reload 后保留，重启时重置。等待量随请求 cleanup 更新。同步操作和核心缓存管理器的删除不计入后台删除总数。字节数是文件长度估计，不代表文件系统块数或已释放磁盘空间；仍打开的读取者及并发写入都可能影响结果。已完成的部分遍历会计入，但崩溃 worker 的结果可能缺失。这些指标不是审计日志。

HIT/MISS 只统计有原生缓存判定的主请求。没有 upstream、缓存状态 0，以及 BYPASS、EXPIRED、STALE、UPDATING、REVALIDATED 都不进入该比率。普通非缓存请求不算 MISS。即使后台清理关闭，仅监控也能创建 SHM 区。32 位系统上的队列年龄适用于约 24.9 天以内的间隔；带符号时间差为负时报告 0。

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

此指令已废弃，仅为配置迁移而保留。`on` 和 `off` 都会产生警告且被忽略，请删除该指令；无效值仍会被拒绝。同步操作没有匹配项时返回 `200 no_match`，接受物理清理时返回 `202 accepted`，发布代际失效时返回 `200 invalidated`。不再支持切换 `412`/`404`。参见 [HTTP 响应契约](../HTTP_RESPONSES.md)。

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

开启后，成功的精确键清理在删除主文件之后遍历缓存目录，删除剩余的、含相同 `KEY:` 字符串的文件。这样可覆盖保存在不同文件路径、但共享同一逻辑键的 `Vary` 和 `gzip_vary` 变体。会检查键后紧接的字节必须为 `\n`，避免误匹配只有共同前缀的其他键。

默认关闭，因为每次精确清理都会增加一次完整目录遍历。存在多个 `Vary`/`gzip_vary` 表示时可启用。即使主条目已不存在，也会遍历并清理变体。通配符和 `purge_all` 已覆盖变体。精确 Vary 键区分大小写，最多 511 字节；超长键在删除任何文件前返回 `400 invalid_selector`。

世代失效无需此选项即可覆盖逻辑键的全部表示。

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

在 location 绑定前声明世代命名空间。名称为 1–32 个 ASCII 字母、数字、`_` 或 `-`；SHM 至少为八个原生页。使用缓存和临时根目录之外的独立持久状态目录。核心通过独占创建的持久标记分配重启 epoch。只要对应缓存可能仍存在，就必须保留这些小文件；保留缓存却删除状态可能导致旧 epoch 被重用。不保证断电／fsync 持久性。普通 reload 保留 epoch 和规则；修改大小、路径或移除命名空间需要完全重启。命名空间 SHM 与任务队列分别指定大小。

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

将 location 绑定到已有命名空间并提供 `$cache_purge_key`。将完整的 `proxy_cache_key`、`fastcgi_cache_key`、`scgi_cache_key` 或 `uwsgi_cache_key` 设为此变量，对应的独立 PURGE location 也必须如此。若展开后的原生键与生成键不同，PURGE 返回 `500`，不会声称完成严格失效。逻辑表达式中不要引用 `$cache_purge_key` 自身。逻辑键最大 8096 字节。若其他 location 需要传统语义，仅绑定参与的 location；没有用于取消继承绑定的 `off` 形式。

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

创建仅接受主请求的显式 REFRESH 端点，其他方法返回 `405`。需要绑定 `cache_purge_key`，原生表达式使用 `$cache_purge_key`，并配置 refresh 配置档。逻辑键选择精确键、末尾 `*` 的前缀，或用 `*` 选择此缓存路径中的整个命名空间。使用核心访问控制／认证保护此 location。REFRESH 仅支持代理缓存，PURGE 仍支持全部四种协议。

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

将一个已知表示映射到固定的内部代理 location。从逻辑键移除大小写敏感的精确前缀后，必须得到以 `/` 开头的 origin URI；查询字符串与转义保留。模块不会猜测主机，也不会在键中搜索 `://`。目标必须启用 `internal`，关闭缓存及请求头／请求体转发，使用 HEAD、非空字面值 Host 和字面值表示头。两个条件头必须分别完整使用 `$cache_purge_refresh_etag` 与 `$cache_purge_refresh_last_modified`。`$cache_purge_refresh_uri` 向 `proxy_pass` 提供映射 URI。这三个无副作用变量仅在探测中有意义。preaccess 会检查目标及内部重定向。管理员必须确保映射指向与缓存键相同的 origin 表示；原生元数据不保存原请求头，因此 Vary 条目会跳过。

在探测 location 中设置 `proxy_intercept_errors off`；继承的错误拦截会被拒绝，防止 origin 错误被改写为触发失效的成功响应。

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

限制 REFRESH 页中新扫描／探测的启动时间，包含等待 worker 扫描器的时间。到期后停止新工作并标记响应不完整。已运行的 HEAD 依赖核心 `proxy_connect_timeout`、`proxy_send_timeout` 和 `proxy_read_timeout` 完成，请显式设置它们。此值不是严格响应期限，也无法中断文件系统调用。断连或 graceful shutdown 会停止后续分派，并保留活动线程的存储直至原生完成回调。

---

<a id="partial-key-purge"></a>

## 部分键清理

在**求值后的缓存键**末尾添加 `*`，按前缀匹配且不区分大小写。精确键和 Vary 匹配仍区分大小写。物理清理前缀最多 511 字节，不含末尾的 `*`。超长前缀只记录一次日志，并返回 `400 invalid_selector`，不遍历文件。

启用 `cache_purge_key` 时匹配逻辑表达式，长度上限为 8096 字节（含末尾 `*`）。

若键表达式以 `$uri` 结尾，以下请求可选择 URI 前缀：

```http
PURGE /images/header*
```

星号必须是求值后整个键的最后一个字节，而不仅仅是 URI 路径的末字节。请考虑 `proxy_cache_key` 的查询参数或变量后缀。键只有 `*` 时会遍历所有符合条件的条目。

---

<a id="generation-invalidation"></a>

## 世代失效

绑定命名空间后，PURGE 先发布新世代并安排去重后的旧世代扫描，再返回 `200` 和 `invalidated`。准入路径不会打开或删除缓存文件。发布后首次计算键的新请求不能 HIT 旧世代。每个请求按命名空间／逻辑键固定世代，跨内部重定向保持；已开始的响应可完成，但迟到的缓存写入不能成为后续新请求的 HIT。精确匹配区分大小写，前缀忽略 ASCII 大小写；`purge_all` 修改整个命名空间，包括有意共享它的其他缓存路径。物理清理仅为目标路径排队，其他路径依赖核心过期清理或后续 PURGE。

接受失效不意味着立即释放磁盘空间。扫描后才完成的旧写入仍不可访问，由后续清理或核心 inactive 过期机制回收。命名空间 SHM 紧张时，规则合并为新的命名空间基线：更多键变冷，而不是让已失效键重新出现。每次后续失效最多回收 64 个已脱离的 trie 节点。反复清理热点键不会无限扩展规则树。复杂度取决于有上限的键长和任务索引大小，而非缓存文件数量；不承诺过载时的墙钟 O(1) 或绝不饥饿。准入可能等待。失效后的 GET 惊群应使用核心 `proxy_cache_lock` 等缓存控制处理。

<a id="conditional-refresh"></a>

## 条件式 REFRESH

REFRESH 在 TTL 到期前验证仍缓存的对象，不预热，也不在 `304` 时延长 TTL。它补充核心 `proxy_cache_revalidate` 和 `proxy_cache_background_update`，后两者负责普通的过期对象流量。如果 origin 忽略验证器并始终返回 `200`，REFRESH 就相当于失效操作加一次 HEAD。候选项仅包括规范哈希路径中当前世代的有效文件。同一世代内物理替换的对象可能被保守地失效；世代比较可防止旧 HEAD 使后续 PURGE 的新世代失效。

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

每个 worker 最多执行一个 REFRESH 页及一个探测，共享额度限制所有 worker 合计最多 16 个探测。其他页请求等待时不分配候选数组。每页通过通用游标扫描目录，保留最多 32 个候选项，再串行执行核心 HEAD 子请求。这同时限制候选内存和共享主请求池的子请求数量。相同 REFRESH HTTP 请求本身不去重，但共享并发限制；其失效操作进入统一的去重 PURGE 队列。

首次请求不带游标，然后用相同端点、逻辑选择器和配置档发送 `X-Cache-Purge-Cursor: <next_cursor>`，直到 `complete` 为 true。游标是最后完成的 32 位十六进制文件名；无效或重复游标头返回 `400`。`scan_complete` 表示目录枚举成功完成；`complete` 还表示没有后续匹配页且已完成所选决定。`timed_out` 和 `scan_error` 区分中断与失败。`undispatched` 是已选中但未完成的数量；扫描不完整时仅为下界，并非缓存总数。若扫描不完整，则不发起探测，结果为空，原样返回输入游标。结果只公开文件哈希和原始 upstream 状态，不返回键或凭据。

每页重新扫描目录以选择下一批哈希，用重复 I/O 换取有界内存：完整操作为 `O(文件数 × 页数)`。并发增删时分页仅具有弱一致性；若需要之后时刻的完整检查，应从头重新开始。请据此选择过滤条件和超时。此管理接口不适合高频反复全量扫描超大型活动缓存。

<a id="generation-and-refresh-example"></a>

### 世代与 REFRESH 示例

```nginx
thread_pool purge_io threads=2 max_queue=32;
http {
    upstream origin { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx/objects levels=1:2 keys_zone=objects:10m;
    cache_purge_namespace objects 1m /var/lib/nginx/purge-epochs;
    cache_purge_thread_pool purge_io;
    cache_purge_response_type json;

    server {
        listen 8080;
        location /objects/ {
            cache_purge_key objects "origin.test$request_uri";
            proxy_cache objects;
            proxy_cache_key $cache_purge_key;
            proxy_cache_lock on;
            proxy_cache_valid 200 10m;
            proxy_set_header Host origin.test;
            proxy_pass http://origin;
            proxy_cache_purge PURGE from 127.0.0.1;
        }
        location = /admin/refresh {
            allow 127.0.0.1; deny all;
            cache_purge_key objects $http_x_cache_key;
            proxy_cache_refresh objects $cache_purge_key;
            cache_purge_refresh_profile /internal/cache-probe origin.test;
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
            proxy_set_header Host origin.test;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since $cache_purge_refresh_last_modified;
            proxy_pass http://origin$cache_purge_refresh_uri;
        }
    }
}
```

```sh
curl -X PURGE 'http://127.0.0.1:8080/objects/item'
curl -X REFRESH -H 'X-Cache-Key: origin.test/objects/*'      'http://127.0.0.1:8080/admin/refresh'
# Repeat with X-Cache-Purge-Cursor set to the returned next_cursor.
```

<a id="sample-configurations"></a>

## 配置示例

<a id="basic--inline-purge-method"></a>

### 基础 — 同一 location 的 PURGE 方法

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/images/logo.png
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match
# 403 — client IP not in the allowed list
```

<a id="basic--separate-purge-location"></a>

### 基础 — 独立清理 location

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass      http://backend;
            proxy_cache     main;
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

<a id="background-queue-enabled"></a>

### 启用后台队列

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

<a id="purge-all-entries"></a>

### 清理全部条目

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location /content {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/content/anything
# 200 — walk completed; inspect error_log for file errors
```

<a id="json-responses-with-per-location-override"></a>

### JSON 响应及 location 级覆盖

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        cache_purge_response_type json;   # default for all locations in this server
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
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

<a id="vary-aware-purge"></a>

### Vary 感知清理

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

清理一个键后，存储在其他文件路径的 gzip 和 Vary 变体也会自动删除。

---

<a id="performance-tuning"></a>

## 性能调优

三个参数控制不同限制：`queue_size` 限制未完成工作，`batch_size` 按 worker 将任务分组，`throttle_ms` 设定组间暂停。修改前后应测量请求延迟、存储竞争和队列年龄。增加队列容量不会提升处理吞吐量。

以下预设只是起始配置，不是硬件性能承诺。列名省略公共前缀 `cache_purge_`。

| 负载／存储预算 | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| 资源受限的主机或存储 | 512 | 5 | 25ms |
| 通用起点 | 2048 | 20 | 10ms |
| 已测得剩余容量的高速存储 | 8192 | 50 | 5ms |
| 短时突发且保守后台处理 | 8192 | 5 | 50ms |

例如，通用预设为：

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

较大队列能够吸收突发，但持续到达率高于完成率仍会将其填满。可用 `max(0, arrival_rate - completion_rate) * burst_seconds` 估算额外所需任务空间，速率应使用去重后实测的任务/秒。为任务时长差异和已有积压预留余量。接纳等待会增加打开的请求与连接；存储饱和时应将其内存和 FD 开销计入预算。

<a id="memory-sizing"></a>

### 内存计算

配置的共享区大小按本机构造体尺寸计算：

```text
Q = cache_purge_queue_size; P = native page size
I = sizeof(ngx_http_cache_purge_queue_item_t)
H = sizeof(ngx_http_cache_purge_queue_t)
A = max(ngx_cacheline_size, 64)
R = align(sizeof(ngx_http_cache_purge_stats_t), A)
S = 2 * (I + 2 * (512 + 1))
M = sum over cache paths of
    2 * (sizeof(ngx_http_cache_purge_path_t) + path_length + 1
         + A + R * NGX_MAX_PROCESSES)
zone_bytes = max(8 * P, align(H + Q * S + M, P))
```

| 组成部分 | 预留或占用内存 |
|---|---|
| 任务预留 | `Q * S`，含 slab 取整、内联键及池名称的余量 |
| 单路径指标 | `R * NGX_MAX_PROCESSES` 加对齐空间；192 字节步长、1024 槽位时为 192 KiB |
| 队列控制 | `H`，含进程所有者和代次数组 |
| 路径预留 | `M`，含名称、rbtree 根、对齐及指标数组 |
| 最小共享区 | 八个本机内存页 |

对实测的 LP64 布局（`I=184`、`H=20576`、路径记录 104 字节、统计记录 144 字节），假设缓存行 64 字节、页大小 4 KiB，且仅有一个长度 20 字节的缓存路径，公式给出：

| 容量 | 共享区字节数 | 共享区 MiB |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

任务的实际存储在 slab 舍入前为 `I + index_bytes + 1 + pool_name_bytes`。内嵌索引包含一个操作标记字节及传统键，或用于世代扫描的命名空间名。传统 `purge_all` 仅存储标记。容量同时约束待执行和执行中的分配。原生结构大小、缓存行、`NGX_MAX_PROCESSES` 和配置路径决定最终用量，Windows 和 32 位布局不同。reload 的保护记录额外占用 SHM，因此分配可能在任务数达到上限前失败。

进程内存单独计算。每个活动遍历使用固定键缓冲区、可复用的路径缓冲区，以及每个打开目录的固定大小栈帧。栈帧只存前缀长度，不复制祖先路径，因此存储为 `O(depth + longest_path)`，与文件总数无关。目录流还具有平台缓冲区。等待请求仅保留小型定时器／cleanup 状态，不持有遍历游标。线程栈和核心线程池有额外内存成本。

命名空间 SHM 使用单独配置的大小。测量的 LP64 布局中，一个 REFRESH 扫描器的固定候选缓冲区／上下文为 283864 字节（32 项），另有遍历帧和 HTTP 请求池内最多 32 个核心子请求。页完成后释放请求，续查使用新请求。其他等待者不分配候选数组。

<a id="throughput-and-latency"></a>

### 吞吐量与延迟

对持续忙碌的 worker，可使用以下近似：

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

忽略 `D` 后仅得到调度上限 `1000 * B / T`，默认是每个 worker 每秒 1000 个任务。这既不是文件删除速率，也不是测量结果。若十个任务共耗时 200 ms，再加相同的 10 ms 暂停，则约为 47.6 任务/秒。worker 共享存储，因此乘以 worker 数并非吞吐保证。空闲 worker 检查新工作前，最多可能等待 `10 * T` 加事件循环延迟。

减小批次或增加暂停，可减少休息间隔之间的工作量。要降低磁盘操作时的 worker 延迟，请使用线程池。这些设置均无法限制单个系统调用，也不会将线程任务切分为多个批次。

---

<a id="monitoring-and-debugging"></a>

## 监控与调试

通过[状态端点](#cache_purge_queue_status)观察待处理工作、活动任务、接纳等待、重复请求、unlink 结果和 HIT/MISS。`202 Accepted` 确认接纳或合并，不证明删除完成或立即逻辑失效。应同时监控请求延迟、队列年龄及存储竞争。

| 级别 | 条件 |
|---|---|
| `warn` | 键过长、任务所用池被移除，或配置值被限制 |
| `error` | 拒绝访问、缓存查找或 REFRESH 配置失败、清理重试耗尽；核心线程池或 Windows 读取失败 |
| `crit` | 文件打开／元数据查询／删除或目录操作失败；POSIX 核心读取失败 |
| `alert` | 文件关闭失败 |
| `emerg` | 无效配置、不兼容 SHM、调整共享区大小，或 reload 元数据分配失败 |
| `debug` | 额外请求和遍历上下文 |

模块诊断使用 NGINX 原生日志函数，消息以 `ngx_cache_purge: ` 开头。严重级别、时间戳、进程／线程 ID、errno 说明及请求或配置上下文由 NGINX 按配置的输出格式添加。文件系统消息标明实际失败的操作及带引号的路径；namespace 和 REFRESH 错误标明对象与拒绝原因。日志采集应使用标准 NGINX 错误日志解析器，并按消息前缀筛选。

调试消息使用 `NGX_LOG_DEBUG_HTTP`，需要同时启用 `--with-debug` 构建及 `error_log /var/log/nginx/error.log debug;`。

队列压力通过接纳等待指标表示，而非每次重试输出警告。文件已消失属于预期竞态，不产生模块诊断。NGINX 核心函数输出的消息（包括配置解析器、内存分配器、I/O 和线程池）保留核心格式；排障时请查看完整日志。

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

已覆盖的诊断见[日志检查](../../t/TESTING.md#logging-contracts)。

---

<a id="troubleshooting"></a>

## 故障排查

| 症状 | 检查或调整 |
|---|---|
| 大遍历期间 worker 延迟升高 | 启用后台队列并使用 I/O 线程池。精确键／Vary 及后台关闭时的操作仍同步执行。 |
| 接纳等待者增加 | 检查到达率、队列年龄、池容量和磁盘负载。增加任务容量只缓冲突发，调整大小需重启。 |
| iowait 高 | 减小 `cache_purge_batch_size`，增大 `cache_purge_throttle_ms`，并限制清理请求流入。 |
| 队列处理慢 | 增大批次或减少暂停前，先测量任务耗时和存储竞争。 |
| 客户端仍期望旧的 `412`/`404` | 删除 `cache_purge_legacy_status`；处理 `200` 和 `Cache-Purge-Code: no_match`。 |
| Vary 变体残留 | 启用 `cache_purge_vary_aware`，检查主文件清理结果及 511 字节变体键上限。 |
| 已接纳工作似乎未完成 | 检查活动任务、恢复和文件错误。`202` 不是完成回执；疑似 SHM 损坏需要重启。 |

---

<a id="testing"></a>

## 测试

在 Docker 中构建并运行全部套件：

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

在 `t/` 中仍可使用原有 Compose 服务：

```sh
docker-compose -f docker-compose.test.yml build nginx-test
docker-compose -f docker-compose.test.yml run --rm nginx-test

docker-compose -f docker-compose.test.yml run --rm test-basic
docker-compose -f docker-compose.test.yml run --rm test-queue
docker-compose -f docker-compose.test.yml run --rm test-config
docker-compose -f docker-compose.test.yml run --rm test-memory
docker-compose -f docker-compose.test.yml run --rm test-performance
docker-compose -f docker-compose.test.yml run --rm test-native
docker-compose -f docker-compose.test.yml run --rm test-runtime
```

使用 Compose 插件时，将 `docker-compose` 换成 `docker compose`。Makefile 会识别两者。`nginx-test` 和 `make test-all` 通过与 CI 相同的 `prove` 入口运行全部七个 `t/*.t` 套件。集成镜像包括原生辅助函数／布局检查和线程池运行时测试。禁用线程的严格构建负责验证增量执行。任何测试失败都会使命令失败。

严格构建、sanitizer、Valgrind、平台作业和报告见 [`t/TESTING.md`](../../t/TESTING.md)。所有 GitHub 作业都定义在 `ci.yml` 中。

---

<a id="migration"></a>

## 迁移

<a id="queue-and-status-changes"></a>

### 队列和状态变更

共享内存布局变更需要完整重启。方法／ACL 语法和协议处理器保持可用。v4 默认使用 JSON，响应头和正文提供一致的结果代码，缺失条目返回 `200 no_match`，`cache_purge_legacy_status` 仅产生警告。请依据 [HTTP 响应契约](../HTTP_RESPONSES.md) 更新客户端。队列容量包含已分配任务；过载请求等待接纳。运行中的重复任务可以预约再次遍历。

状态增加字段，同时保留现有字段名。`requests.overflow` 现统计接纳等待者。指标为近似值，累计计数器为 64 位。支持线程的构建在引用时自动注册 `default`；要隔离 PURGE I/O，请选择命名池。仅配置状态端点也会分配 SHM。

v4.0 的世代失效与 REFRESH 均需显式启用。在所有参与的 location 中绑定 `cache_purge_key`，并将整个原生缓存键设为 `$cache_purge_key`。这会改变物理文件名，已有传统文件不再 HIT，保留至核心过期清理或显式传统清理。状态标记必须与缓存一同保留。原有 PURGE location 不会自动获得 REFRESH。实验性 PR60 用户必须配置新端点、配置档和分页；PR60 的无界响应、猜测 URI/Host 及隐式方法扩展不属于兼容承诺。

<a id="from-the-original-frickle-module"></a>

### 从原始 FRiCKLE 模块迁移

方法／访问列表和区域／键两种形式保持可用。缺失条目返回 `200 no_match`；请删除 `cache_purge_legacy_status` 并更新客户端。启用后台处理时添加：

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### 从 nginx-selective-cache-purge-module 迁移

替换模块即可，后台队列功能已经内置。将原来的队列参数映射为 `cache_purge_queue_size`、`cache_purge_batch_size` 和 `cache_purge_throttle_ms`。

---

<a id="temporary-directories"></a>

## 临时目录

遍历按完整路径组件比较，跳过缓存根下已配置的临时目录。未配置为临时路径时，`proxy_temp` 之类名称并非保留名。只有文件名恰为 32 个十六进制字符的普通文件才是删除候选。数字名和带后缀的缓存写入临时文件会跳过，包括 `use_temp_path=off` 的写入。

所有扫描还会验证原生头版本、偏移、完整 `KEY: ` 标记、CRC32 及键／变体文件名哈希，键上限固定为 8192 字节。即使 purge-all 也会跳过损坏、截断、版本不符或不匹配的文件。ETag/Vary 长度等于核心上限时允许通过。格式验证不能证明恶意文件可信。

配置的缓存根目录可为符号链接或 Windows junction；子级符号链接／重解析点会跳过。POSIX 在完整功能探测通过后，使用核心 `ngx_openat_file`／`ngx_file_at_info` 及 no-follow 标记访问子目录和文件，并相对已持有的父目录删除。这可防止子级链接被替换后重定向路径，但不是防御已打开目录被移动的沙箱。Windows 和旧 Unix 使用核心完整路径操作及链接检查，仍存在路径替换竞争。传统精确键的核心查找／删除也使用完整路径。缓存根目录应保持私有，不允许不可信写入者。

配置路径应采用统一、规范的拼写。比较不会解析符号链接、挂载或 Unicode 别名。任务保留保护集合，新的重复请求更新等待任务或下一轮重跑；当前运行的遍历保留原集合。这不是所有存活 worker 代次设置的并集。相关任务仍待处理或运行时，不要改变这些路径的用途。

---

<a id="security"></a>

## 安全

- 通过 `allow` / `deny` 或前置认证限制 PURGE 端点，切勿公开暴露。
- 根据流量配置 `cache_purge_queue_size`，限制请求洪峰中的共享内存消耗。
- 可为清理 location 配合 `limit_req`，独立于普通流量限制清理请求速率。

---

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

---

<a id="see-also"></a>

## 另请参阅

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)
