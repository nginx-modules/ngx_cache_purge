# 发行版软件包

下列下游软件包和 ports 均引用本项目的
[`ngx_cache_purge`](https://github.com/nginx-modules/ngx_cache_purge) 分支。它们由各自的
发行版或社区项目维护，版本、源码修订和所支持的功能会随发布版与分支而不同。

**列表中的软件包不一定包含 v4。** 在依赖 generation 键失效或 REFRESH 之前，请先核对该软件包的版本、
构建配方和变更记录。这里的链接只用于说明下游打包情况，不构成任何发行版的认证或支持承诺。

## 软件包与 ports

| 系统 | 软件包或 port | 打包来源与说明 |
| --- | --- | --- |
| Debian | [`libnginx-mod-http-cache-purge`](https://packages.debian.org/trixie/libnginx-mod-http-cache-purge) | [包跟踪](https://tracker.debian.org/pkg/libnginx-mod-http-cache-purge)；[unstable 包](https://packages.debian.org/sid/libnginx-mod-http-cache-purge) |
| Ubuntu 26.04 LTS | [`libnginx-mod-http-cache-purge`](https://packages.ubuntu.com/resolute/libnginx-mod-http-cache-purge) | Universe 仓库；请核对你所用 Ubuntu 版本的软件包 |
| Arch Linux | [`nginx-mod-cache_purge`](https://archlinux.org/packages/extra/x86_64/nginx-mod-cache_purge/) | Extra 仓库；[PKGBUILD](https://gitlab.archlinux.org/archlinux/packaging/packages/nginx-mod-cache_purge/-/blob/main/PKGBUILD) |
| Arch Linux AUR | [`nginx-mainline-mod-cache_purge`](https://aur.archlinux.org/packages/nginx-mainline-mod-cache_purge) | 社区维护的 `nginx-mainline` 配方；[PKGBUILD](https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=nginx-mainline-mod-cache_purge) |
| Alpine Linux | [`nginx-mod-http-cache-purge`](https://pkgs.alpinelinux.org/package/edge/main/x86/nginx-mod-http-cache-purge) | [APKBUILD](https://gitlab.alpinelinux.org/alpine/aports/-/blob/master/main/nginx/APKBUILD)；请选择你使用的分支与架构 |
| FreeBSD / NGINX | [`www/nginx-module-cache-purge`](https://cgit.freebsd.org/ports/tree/www/nginx-module-cache-purge) | [Port Makefile](https://cgit.freebsd.org/ports/tree/www/nginx-module-cache-purge/Makefile)；为 NGINX ports 提供源码 |
| FreeBSD / Angie | [`www/angie-module-cache-purge`](https://cgit.freebsd.org/ports/tree/www/angie-module-cache-purge) | [Port Makefile](https://cgit.freebsd.org/ports/tree/www/angie-module-cache-purge/Makefile)；Angie 动态模块 |

## 版本与二进制兼容性

仅凭软件包名称无法判断上游版本。较早的软件包可能基于不同的上游；Debian 从软件包版本
`1:2.5.3-1` 起才切换到本分支。基于 2.x 的软件包不包含 3.x 的后台队列，基于 3.x 的软件包
不包含 v4 的 generation 键或 REFRESH。请以发行版实际提供的源码和版本为准。

NGINX 动态模块必须与目标服务器的版本、构建选项和 ABI 要求匹配。不要假设为某个 NGINX
版本或操作系统构建的软件包可以被另一个加载。参见[动态模块兼容性](BINARY_MODULES.zh-CN.md)
和[迁移要求](MIGRATION.zh-CN.md)。

若使用由本项目（而非下游维护者）构建的模块，请查看
[GitHub releases](https://github.com/nginx-modules/ngx_cache_purge/releases) 及其记录中的构建要求。
