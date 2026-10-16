# ngx_cache_purge

[README](README.zh-CN.md) · [指令](DIRECTIVES.zh-CN.md)

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
