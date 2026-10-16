# Distribution packages

The downstream packages and ports below reference this fork of
[`ngx_cache_purge`](https://github.com/nginx-modules/ngx_cache_purge). They are
maintained by their respective distribution or community projects. Their
versions, source revisions and supported features vary by release and branch.

**A listed package does not necessarily contain v4.** Check its package version,
source recipe and changelog before relying on generation-key invalidation or
REFRESH. These links document downstream packaging, not a certification or
support commitment from a distribution.

## Packages and ports

| System | Package or port | Packaging source and details |
| --- | --- | --- |
| Debian | [`libnginx-mod-http-cache-purge`](https://packages.debian.org/trixie/libnginx-mod-http-cache-purge) | [Package tracker](https://tracker.debian.org/pkg/libnginx-mod-http-cache-purge); [unstable package](https://packages.debian.org/sid/libnginx-mod-http-cache-purge) |
| Ubuntu 26.04 LTS | [`libnginx-mod-http-cache-purge`](https://packages.ubuntu.com/resolute/libnginx-mod-http-cache-purge) | Universe; check the package for your Ubuntu release |
| Arch Linux | [`nginx-mod-cache_purge`](https://archlinux.org/packages/extra/x86_64/nginx-mod-cache_purge/) | Extra repository; [PKGBUILD](https://gitlab.archlinux.org/archlinux/packaging/packages/nginx-mod-cache_purge/-/blob/main/PKGBUILD) |
| Arch Linux AUR | [`nginx-mainline-mod-cache_purge`](https://aur.archlinux.org/packages/nginx-mainline-mod-cache_purge) | Community-maintained recipe for `nginx-mainline`; [PKGBUILD](https://aur.archlinux.org/cgit/aur.git/tree/PKGBUILD?h=nginx-mainline-mod-cache_purge) |
| Alpine Linux | [`nginx-mod-http-cache-purge`](https://pkgs.alpinelinux.org/package/edge/main/x86/nginx-mod-http-cache-purge) | [APKBUILD](https://gitlab.alpinelinux.org/alpine/aports/-/blob/master/main/nginx/APKBUILD); select the branch and architecture you use |
| FreeBSD / NGINX | [`www/nginx-module-cache-purge`](https://cgit.freebsd.org/ports/tree/www/nginx-module-cache-purge) | [Port Makefile](https://cgit.freebsd.org/ports/tree/www/nginx-module-cache-purge/Makefile); supplies sources for the NGINX ports |
| FreeBSD / Angie | [`www/angie-module-cache-purge`](https://cgit.freebsd.org/ports/tree/www/angie-module-cache-purge) | [Port Makefile](https://cgit.freebsd.org/ports/tree/www/angie-module-cache-purge/Makefile); dynamic Angie module |

## Version and binary compatibility

Package names alone do not identify the upstream version. Earlier packages may
use a different upstream; Debian switched to this fork in package version
`1:2.5.3-1`. Packages based on 2.x do not include the 3.x background queue,
and packages based on 3.x do not include v4 generation keys or REFRESH. Review
the exact source and version provided by your distribution.

An NGINX dynamic module must match the target server's version, build options
and ABI requirements. Do not assume that a package built for one NGINX release
or operating system can be loaded by another. See [dynamic module
compatibility](BINARY_MODULES.md) and [migration requirements](MIGRATION.md).

For modules built by this project rather than a downstream maintainer, see the
[GitHub releases](https://github.com/nginx-modules/ngx_cache_purge/releases)
and their recorded build requirements.
