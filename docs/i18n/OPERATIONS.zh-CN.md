# ngx_cache_purge

[README](README.zh-CN.md) · [指令](DIRECTIVES.zh-CN.md)

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

<a id="reading-cache-files"></a>

### 在世代模式下读取缓存文件

缓存浏览器、预加载器或用量报告无法仅凭文件判断世代是否有效：失效后旧文件仍可能存在，即使尚无替代文件。世代文件的 `KEY:` 行为 `p4:<namespace>:<epoch>:<generation>:<logical key>`。[状态端点](DIRECTIVES.zh-CN.md#cache_purge_queue_status)提供命名空间状态。键的当前世代为 `baseline`，除非精确匹配或前缀规则已将其提升；响应不包含这些规则本身。

| 文件所属命名空间的状态 | 判定 |
|---|---|
| 命名空间不在列表中 | `unknown` |
| 文件的 epoch 与 `epoch` 不同 | `stale` |
| Epoch 相同；文件世代小于 `baseline` | `stale` |
| Epoch 相同；世代不小于 `baseline`；`rules` 为 `false` | `live` |
| Epoch 相同；世代不小于 `baseline`；`rules` 为 `true` | `unknown` |

这里的 **live** 仅表示文件未因命名空间世代而过时，并不证明内容新鲜、TTL 有效或 NGINX 缓存会 HIT。`namespaces_complete=false` 时，已返回的条目仍可使用，被省略的命名空间才属于未知。

如果另一个文件的命名空间、epoch 和逻辑键相同，但世代更高，就能证明较低世代的文件已过时：键的当前世代在同一 epoch 内不会下降。其他情况下，应保留 unknown 判定。

扫描前后各读取一次状态，按命名空间名称匹配条目。只有命名空间同时出现在两次响应中，且 `epoch`、`baseline`、`serial` 均未变化时，才使用基于状态的判定。状态缺失或变化时，应丢弃该命名空间的判定。Epoch 只比较是否相等。这些观测并不能阻止后续失效。

清理队列为空不能证明有效。失效前开始的请求可能在清理完成后写入旧世代文件；文件会保留到后续清理或原生缓存淘汰时。此外，清理只针对 PURGE location 的缓存路径，而一个命名空间可能由多条路径共享。

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

通过[状态端点](DIRECTIVES.zh-CN.md#cache_purge_queue_status)观察待处理工作、活动任务、接纳等待、重复请求、unlink 结果和 HIT/MISS。`202 Accepted` 确认接纳或合并，不证明删除完成或立即逻辑失效。应同时监控请求延迟、队列年龄及存储竞争。

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

<a id="temporary-directories"></a>

## 临时目录

遍历按完整路径组件比较，跳过缓存根下已配置的临时目录。未配置为临时路径时，`proxy_temp` 之类名称并非保留名。只有文件名恰为 32 个十六进制字符的普通文件才是删除候选。数字名和带后缀的缓存写入临时文件会跳过，包括 `use_temp_path=off` 的写入。

所有扫描还会验证原生头版本、偏移、完整 `KEY: ` 标记、CRC32 及键／变体文件名哈希，键上限固定为 8192 字节。即使 purge-all 也会跳过损坏、截断、版本不符或不匹配的文件。ETag/Vary 长度等于核心上限时允许通过。格式验证不能证明恶意文件可信。

配置的缓存根目录可为符号链接或 Windows junction；子级符号链接／重解析点会跳过。POSIX 在完整功能探测通过后，使用核心 `ngx_openat_file`／`ngx_file_at_info` 及 no-follow 标记访问子目录和文件，并相对已持有的父目录删除。这可防止子级链接被替换后重定向路径，但不是防御已打开目录被移动的沙箱。Windows 和旧 Unix 使用核心完整路径操作及链接检查，仍存在路径替换竞争。传统精确键的核心查找／删除也使用完整路径。缓存根目录应保持私有，不允许不可信写入者。

配置路径应采用统一、规范的拼写。比较不会解析符号链接、挂载或 Unicode 别名。任务保留保护集合，新的重复请求更新等待任务或下一轮重跑；当前运行的遍历保留原集合。这不是所有存活 worker 代次设置的并集。相关任务仍待处理或运行时，不要改变这些路径的用途。

---

<a id="testing"></a>

## 测试

[测试覆盖说明](../TEST_COVERAGE.md)（英文）列出了已验证的行为与尚未覆盖的情况。

在 Docker 中构建并运行全部套件：

```sh
make test-all
NGINX_VERSION=1.31.7 make test-all
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

严格构建、sanitizer、Valgrind、平台作业和报告见 [`t/TESTING.md`](../../t/TESTING.md)。所有 GitHub 作业都定义在 `build-test-release.yml` 中。
