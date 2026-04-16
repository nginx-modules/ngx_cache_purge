NGINX_VERSION ?= 1.25.5
NGINX_SRC_DIR ?= /opt/nginx-src/nginx-$(NGINX_VERSION)
NGINX_BUILD_PREFIX ?= /opt/nginx
MODULE_DIR ?= /workspace
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
DOCKER_COMPOSE ?= docker compose

.PHONY: help image shell nginx-build nginx-build-dynamic nginx-version format test

help:
	@printf '%s\n' \
		'make shell            Open a shell in the development container' \
		'make nginx-build      Build NGINX with this module' \
		'make nginx-build-dynamic Build this module as objs/ngx_http_cache_purge_module.so in the dev container' \
		'make nginx-version    Build info for the installed NGINX binary' \
		'make format           Run the repository formatter' \
		'make test             Run the Test::Nginx suite'

shell:
	$(DOCKER_COMPOSE) run --rm dev

nginx-build:
	test -d "$(NGINX_SRC_DIR)"
	cd "$(NGINX_SRC_DIR)" && ./configure \
		--prefix="$(NGINX_BUILD_PREFIX)" \
		--with-debug \
		--with-http_ssl_module \
		--add-module="$(MODULE_DIR)"
	$(MAKE) -C "$(NGINX_SRC_DIR)" -j"$(JOBS)"
	$(MAKE) -C "$(NGINX_SRC_DIR)" install

nginx-build-dynamic:
	test -d "$(NGINX_SRC_DIR)"
	cd "$(NGINX_SRC_DIR)" && ./configure \
		--prefix="$(NGINX_BUILD_PREFIX)" \
		--with-compat \
		--with-debug \
		--with-http_ssl_module \
		--with-ld-opt="-lsqlite3" \
		--add-dynamic-module="$(MODULE_DIR)"
	$(MAKE) -C "$(NGINX_SRC_DIR)" -j"$(JOBS)" modules

nginx-version:
	"$(NGINX_BUILD_PREFIX)/sbin/nginx" -V

format:
	astyle -v --options=.astylerc ./*.c
	dos2unix ./*.c

test:
	$(MAKE) nginx-build >/tmp/nginx-build.log
	@set -e; \
	for test_file in t/*.t; do \
		rm -rf /tmp/ngx_cache_*; \
		echo "== $$test_file =="; \
		TEST_NGINX_BINARY="$(NGINX_BUILD_PREFIX)/sbin/nginx" prove "$$test_file" || exit $$?; \
	done
