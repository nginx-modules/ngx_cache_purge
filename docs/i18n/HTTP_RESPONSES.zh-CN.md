# v4 的 HTTP 响应

HTTP 状态描述请求结果，`Cache-Purge-Code` 标识模块结果；JSON 正文的 `code` 字段与之相同。模块响应均带 `Cache-Control: no-store`。客户端应根据状态码和 `code` 分支处理，不要解析面向人的提示文本。

## 完成与接纳

`200 OK` 表示所请求的同步操作或逻辑失效已经完成。`202 Accepted` 表示物理清理已被接纳或合并，但稍后仍可能失败。两者都不承诺并发写入者会让缓存永久为空。这些含义遵循 [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-15.3)。

generation 失效返回 `200 invalidated`：发布之后旧 generation 已不可达，而物理清理仍在等待执行。普通的后台通配符／整缓存清理返回 `202 accepted`。队列已满时请求会等待准入，不会立刻返回 `202`，也不会因过载退化为同步遍历。

状态端点报告的是聚合的队列与清理计数器，而不是逐请求的任务监视器：v4 不发放任务 ID，也不提供任务的 `Location`。请检查清理错误和重试耗尽情况；请求被接纳并不能证明每个文件都已删除。

## 结果目录

| HTTP | `Cache-Purge-Code` / JSON `code` | 含义 |
|---|---|---|
| 200 | `purged` | 匹配的文件已同步删除 |
| 200 | `no_match` | 同步操作未找到匹配条目 |
| 202 | `accepted` | 物理清理已被接纳或合并 |
| 200 | `invalidated` | 已发布逻辑失效并接纳清理 |
| 200 | `status` | 聚合状态快照 |
| 200 | `refresh_complete` | REFRESH 的扫描／页面处理已完成；请查看逐条结果 |
| 200 | `refresh_incomplete` | REFRESH 需要续页或已到达时间上限 |
| 400 | `invalid_selector` | 选择条件超过支持的键长度 |
| 400 | `invalid_cursor` | REFRESH 游标不是单个 32 位十六进制值 |
| 403 | `access_denied` | 被模块访问列表拒绝 |
| 405 | `method_not_allowed` | 专用端点使用了错误的方法；`Allow` 列出可用方法 |
| 500 | `cache_unavailable` | 缓存配置无法用于本次操作 |
| 500 | `key_mismatch` | 原生缓存键违反所配置的命名空间契约 |
| 500 | `delete_failed` | 同步删除失败，可能已部分删除 |
| 500 | `scan_failed` | 同步 PURGE 或 REFRESH 扫描失败，可能已有部分效果 |
| 500 | `internal_error` | 其它模块故障或未知的内部结果 |

条目缺失属于成功的幂等结果（`200 no_match`），不是失败的 HTTP 前置条件。对不存在的键做 generation 失效仍返回 `invalidated`。已完成的 REFRESH 仍可能包含个别条目的源站／传输错误，请查看其 `results` 和续页字段。源站状态策略、元数据限制、取消与重试行为见 [REFRESH 语义与边界情况](REFRESH.zh-CN.md)。目录扫描失败返回 `500 scan_failed`，不会返回普通的续页文档。

物理通配符前缀上限为 511 字节（不含末尾 `*`）。带 Vary 感知遍历的精确键同样受 511 字节限制。命名空间选择条件允许 8096 字节（含末尾 `*`）。超长选择条件在删除前即被拒绝；物理 `purge_all` 不需要前缀。原生缓存文件校验另有 8192 字节的键长度限制。

模块不会翻译在其处理器之前由 NGINX 请求解析、认证、访问阶段或其它模块产生的错误。核心的请求体丢弃错误同样沿用核心处理。内存耗尽和连接关闭可能导致连错误响应都无法送达。自定义过滤器或内部重定向可能改写响应。本目录只覆盖模块自身产生的终结结果，不覆盖所有可能的 NGINX／网络故障。

## 正文与格式

`cache_purge_response_type json` 为默认值。成功的 PURGE JSON 使用小写字段名：

```json
{"key":"/items/42","key_encoding":"utf-8","code":"no_match","complete":true,"cleanup_pending":false,"deleted":0,"delete_errors":0,"scan_errors":0}
```

`complete` 描述本次操作，而不是物理清理：`invalidated` 的 `complete` 为 true，其 `cleanup_pending` 同样为 true；`accepted` 的 `complete` 为 false、`cleanup_pending` 为 true。只有当本次请求执行了同步遍历／删除时才会带上三个计数器，排队的工作不会伪造这些计数。

`html`、`xml` 和 `text` 仍是显式的成功格式，各自包含键、结果码和键编码；XML 使用 `application/xml`。键会按所选表示形式转义。二进制键、非法 UTF-8 或 XML 1.0 之外的字符在所有格式中都会无损地以 Base64 编码，并标注 `base64`；合法文本标注 `utf-8`。

模块错误统一使用 [RFC 9457](https://www.rfc-editor.org/rfc/rfc9457.html) 的 `application/problem+json`，与所选的成文格式无关：

```http
HTTP/1.1 405 Method Not Allowed
Allow: PURGE
Cache-Purge-Code: method_not_allowed
Cache-Control: no-store
Content-Type: application/problem+json

{"type":"about:blank","title":"Method Not Allowed","status":405,"detail":"Use a method listed in the Allow header.","code":"method_not_allowed"}
```

`type: about:blank` 保留标准 HTTP 问题的含义；`code` 是模块扩展。`status` 与 HTTP 状态一致。详情不会暴露文件系统路径、凭据或原始内部诊断信息。同步扫描／删除失败时会附带已知的部分操作计数器。

`cache_purge_response_type none` 会抑制 PURGE 正文：成功的 `200` 变为 `204 No Content`，被接纳的工作仍为 `202`，错误保留其 4xx/5xx 状态，`Cache-Purge-Code` 仍然可用。HEAD 按普通 HTTP 规则省略正文。无论 PURGE 成功格式如何，状态端点与 REFRESH 端点都保留结构化 JSON。

## 从 v2/v3 迁移

请移除 `cache_purge_legacy_status`。`on` 和 `off` 仅为了便于迁移而继续被接受，会输出配置警告但不产生任何效果；非法值会被拒绝。v4 不再用该指令选择 `412` 或 `404`，也不会因为指令过时而发送 HTTP `Deprecation` 或 `Sunset` 响应头。

客户端需适配：默认 JSON、JSON 字段小写、以 `accepted` 取代旧的 `queued` 标签，以及 `200 no_match`。需要时请显式指定成功格式；格式选择不会恢复旧的状态码策略。
