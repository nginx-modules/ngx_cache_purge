# 动态模块

请针对目标服务器所使用的 NGINX 源码版本构建模块：

```sh
nginx -V
cd /path/to/matching-nginx-source
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

把 `objs/ngx_http_cache_purge_module.so` 复制到服务器的模块目录，在主配置上下文（`events` 和 `http` 之前）加入 `load_module`，然后在 reload 之前检查配置：

```nginx
load_module modules/ngx_http_cache_purge_module.so;
```

```sh
nginx -t
```

请使用 `nginx -V` 显示的目标服务器相关 configure 选项。`--with-compat` 从 NGINX 1.11.5 起可用；更早的动态模块目标需要使用其原始的匹配构建选项。模块的静态构建最低版本要求不变。

## 兼容性的含义

`--with-compat` 只处理 NGINX 构建选项中的部分差异。它不会取消 NGINX 版本检查，也不保证跨 CPU 架构、C 库、发行版补丁或任意 NGINX 衍生版本可用。为 NGINX 1.31.7 构建的二进制并不适用于所有 1.31.x 版本。请始终在实际的目标安装上执行 `nginx -t`。

NGINX Open Source 是开源服务器；NGINX Plus 是 F5 NGINX One 中的商业服务器。对 Plus，请先用 `nginx -v` 确认其底层 NGINX 版本，并遵循 [F5 的第三方模块构建说明](https://www.f5.com/company/blog/nginx/compiling-dynamic-modules-nginx-plus)。本项目的发布产物使用 NGINX Open Source 构建并做加载测试，未经 NGINX Plus 认证或测试；不要从 stable 或 mainline 分支的名称推断 Plus 兼容性。

## 发布归档

发布矩阵固定一个当前 stable 版本和一个当前 mainline 版本，分别在 Linux x86_64 与 aarch64 上原生构建。发布过程中不会下载会变化的 `latest` 版本。每次发版前请复核这些固定版本；使用其它 NGINX 版本的用户应自行从匹配的源码构建。

归档命名示例：

- `ngx_cache_purge-4.0.0-nginx-1.31.7-linux-x86_64.tar.gz`
- `ngx_cache_purge-4.0.0-nginx-1.31.7-linux-aarch64.zip`

两种格式包含相同的模块、许可证、说明、模块的 `SHA256SUMS` 和 `BUILD-INFO.txt`。构建信息记录提交、源码归档哈希、configure 选项、编译器、构建操作系统、glibc 与 ELF 依赖。构建使用固定版本的 Ubuntu 24.04 runner 与 glibc。操作系统版本不出现在归档名中，但会记录在兼容性信息里。这些不是 Alpine/musl 二进制；musl 需要单独构建并使用明确区分的产物名。

请同时下载发布级的 `SHA256SUMS` 以校验归档哈希。这些哈希用于发现损坏，不是独立签名。安装到其它发行版之前，请先阅读记录中的环境要求。

## 发布不可变版本

推送一个已存在的 `4.*` 标签（例如不带 `v` 前缀的 `4.0.0`）即可启动 CI。必需的 CI 任务通过后，四个原生任务会编译 NGINX 与动态模块、确认 NGINX 能加载模块并接受 purge 配置，并产出八个归档。发布任务需要完整的一套产物：创建或续用草稿、上传归档与校验和、再下载回来验证哈希，最后才发布。

在此工作流完成之前不要手动发布。使用[不可变发布](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository)时，产物必须在发布前附加。上传失败会留下可重试的草稿；重跑不会修改已发布的版本。草稿中出现的意外产物会阻止发布，而不是被静默纳入发布。

标签必须由用户或其它能触发工作流的凭据推送。用工作流默认 `GITHUB_TOKEN` 创建的标签不会触发另一个 push 工作流。在 GitHub 网页上直接创建 release 不能替代这条标签推送流程。

## 可重复构建与来源证明

发布矩阵的每个任务都会在不同临时目录中执行两次干净构建，并比较两种归档格式的 SHA-256。不一致会使任务失败并阻止发布。`Rebuild check` 任务及其摘要会显示结果与归档哈希。

构建把提交时间戳用作 `SOURCE_DATE_EPOCH`，规范化编译器源码路径，并固定归档条目顺序、时间戳和属主。`BUILD-INFO.txt` 记录编译器、链接器、打包工具、所安装软件包的版本以及源码哈希。若要在相同的兼容 Linux 构建环境中本地复现检查，请在模块检出目录执行：

```sh
export NGINX_VERSION=1.31.7 MODULE_VERSION=4.0.0
bash tools/build-release-module.sh
bash tools/verify-release-rebuild.sh
```

这是同一环境下的重复构建检查。托管 runner 镜像与 APT 仓库都可能变化；仅凭版本标签和记录的软件包版本并不能冻结该环境。日后若要独立复现，需要保留以镜像摘要和依赖快照固定的构建环境。当前检查不承诺这一点。

[构建来源证明](https://docs.github.com/en/actions/concepts/security/artifact-attestations)标明产出归档的源提交与 GitHub Actions 工作流。可用以下命令校验下载的归档：

```sh
gh attestation verify ngx_cache_purge-4.0.0-nginx-1.31.7-linux-x86_64.tar.gz \
    -R nginx-modules/ngx_cache_purge
```

[发布不可变性](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases)可防止已发布的标签和产物被修改，并提供发布证明。不可变性和来源证明都不能证明逐字节可重复；证明在重建检查之后生成，也不会嵌入被比较的归档中。
