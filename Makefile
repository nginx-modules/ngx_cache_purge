NGINX_VERSION ?= 1.25.5
NGINX_SRC_DIR ?= /opt/nginx-src/nginx-$(NGINX_VERSION)
NGINX_BUILD_PREFIX ?= /opt/nginx
MODULE_DIR ?= /workspace # TODO: make this relative to the project.
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
DOCKER_COMPOSE ?= docker compose

.PHONY: help image shell nginx-build nginx-build-dynamic nginx-version format test bench bench-quick

help:
	@printf '%s\n' \
		'make shell               Open a shell in the development container' \
		'make nginx-build         Build NGINX with this module' \
		'make nginx-build-dynamic Build this module as objs/ngx_http_cache_pilot_module.so' \
		'make nginx-version       Build info for the installed NGINX binary' \
		'make format              Run the repository formatter' \
		'make test                Run the Test::Nginx suite' \
		'make bench               Run full benchmark suite (60s per scenario)' \
		'make bench-quick         Run abbreviated benchmark suite (15s per scenario)'

shell:
	$(DOCKER_COMPOSE) run --rm dev

nginx-build:
	test -d "$(NGINX_SRC_DIR)"
	cd "$(NGINX_SRC_DIR)" && ./configure \
		--prefix="$(NGINX_BUILD_PREFIX)" \
		--with-debug \
		--with-threads \
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
		--with-threads \
		--with-http_ssl_module \
		--add-dynamic-module="$(MODULE_DIR)"
	$(MAKE) -C "$(NGINX_SRC_DIR)" -j"$(JOBS)" modules

nginx-version:
	"$(NGINX_BUILD_PREFIX)/sbin/nginx" -V

format:
	astyle -v --options=.astylerc src/*.c src/*.h
	dos2unix src/*

test:
	$(MAKE) nginx-build
	TEST_NGINX_BINARY="$(NGINX_BUILD_PREFIX)/sbin/nginx" prove ./t

bench: nginx-build
	perl ./bench/bench.pl \
		--port 18080 \
		--out-dir ./bench/results

bench-quick: nginx-build
	perl ./bench/bench.pl \
		--quick \
		--port 18080 \
		--out-dir ./bench/results
