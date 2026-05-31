NGINX_VERSION ?= 1.25.5
NGINX_SRC_DIR ?= /opt/nginx-src/nginx-$(NGINX_VERSION)
NGINX_BUILD_PREFIX ?= /opt/nginx
MODULE_DIR ?= $(CURDIR)
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
DOCKER_COMPOSE ?= docker compose
DEBIAN_BUILD_ROOT ?= $(CURDIR)/.pkg-build
DEBIAN_SOURCE_PACKAGE ?= libnginx-mod-http-cache-pilot
DEBIAN_DISTRIBUTION ?=
DEBIAN_VERSION_SUFFIX ?=
LAUNCHPAD_PPA ?= ppa:wpelevator/packages

.PHONY: help image shell packaging-shell nginx-build nginx-build-dynamic nginx-version format test bench bench-quick debian-package debian-package-smoke debian-package-clean debian-source-package debian-source-package-signed launchpad-ppa-upload

help:
	@printf '%s\n' \
		'make shell               Open a shell in the development container' \
		'make packaging-shell     Open a shell in the packaging container' \
		'make nginx-build         Build NGINX with this module' \
		'make nginx-build-dynamic Build this module as objs/ngx_http_cache_pilot_module.so' \
		'make nginx-version       Build info for the installed NGINX binary' \
		'make format              Run the repository formatter' \
		'make test                Run the Test::Nginx suite' \
		'make debian-package      Build Debian source and binary packages under .pkg-build/' \
		'make debian-package-smoke Build Debian packages and run debian/tests/smoke' \
		'make debian-source-package Build unsigned Debian source package under .pkg-build/' \
		'make debian-source-package-signed Build signed Debian source package under .pkg-build/' \
		'make launchpad-ppa-upload Build, sign, and upload source package to Launchpad PPA' \
		'make bench               Run full benchmark suite (60s per scenario)' \
		'make bench-quick         Run abbreviated benchmark suite (15s per scenario)'

shell:
	$(DOCKER_COMPOSE) run --rm dev

packaging-shell:
	$(DOCKER_COMPOSE) run --rm packaging

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

debian-package-clean:
	rm -rf "$(DEBIAN_BUILD_ROOT)"

debian-source-package:
	package_version="$$(dpkg-parsechangelog -l "$(CURDIR)/debian/changelog" -SVersion)"; \
	source_version="$${package_version}$(DEBIAN_VERSION_SUFFIX)"; \
	upstream_version="$$(printf '%s\n' "$$package_version" | sed 's/-[^-]*$$//')"; \
	source_package="$(DEBIAN_SOURCE_PACKAGE)"; \
	build_root="$(DEBIAN_BUILD_ROOT)"; \
	source_dir="$$build_root/$${source_package}-$${upstream_version}"; \
	orig_tarball="$$build_root/$${source_package}_$${upstream_version}.orig.tar.gz"; \
	rm -rf "$$build_root"; \
	mkdir -p "$$source_dir"; \
	tar \
		--exclude-vcs \
		--exclude=".pkg-build" \
		--exclude=".nginx-debug" \
		--exclude="bench/results" \
		--exclude="debian/build" \
		--exclude="debian/.debhelper" \
		--exclude="debian/files" \
		--exclude="*.buildinfo" \
		--exclude="*.changes" \
		--exclude="*.deb" \
		--exclude="*.ddeb" \
		--exclude="*.debian.tar.*" \
		--exclude="*.dsc" \
		--exclude="*.orig.tar.gz" \
		-cf - . | tar -xf - -C "$$source_dir"; \
	tar -C "$$build_root" -czf "$$orig_tarball" "$${source_package}-$${upstream_version}"; \
	changelog_distribution="$$(dpkg-parsechangelog -l "$(CURDIR)/debian/changelog" -SDistribution)"; \
	source_distribution="$${DEBIAN_DISTRIBUTION:-$$changelog_distribution}"; \
	if [ "$$source_version" != "$$package_version" ] || [ "$$source_distribution" != "$$changelog_distribution" ]; then \
		cd "$$source_dir" && \
			DEBFULLNAME="$${DEBFULLNAME:-WPElevator Packaging Team}" \
			DEBEMAIL="$${DEBEMAIL:-hi@wpelevator.com}" \
			dch --newversion "$$source_version" \
				--distribution "$$source_distribution" \
				--force-distribution \
				"Build for $$source_distribution."; \
	fi; \
	cd "$$source_dir" && dpkg-buildpackage -S -sa -us -uc

debian-package: debian-source-package
	package_version="$$(dpkg-parsechangelog -l "$(CURDIR)/debian/changelog" -SVersion)"; \
	upstream_version="$$(printf '%s\n' "$$package_version" | sed 's/-[^-]*$$//')"; \
	source_package="$(DEBIAN_SOURCE_PACKAGE)"; \
	source_dir="$(DEBIAN_BUILD_ROOT)/$${source_package}-$${upstream_version}"; \
	cd "$$source_dir" && dpkg-buildpackage -b -us -uc

debian-package-smoke: debian-package
	apt-get update
	apt-get install -y --no-install-recommends "$(DEBIAN_BUILD_ROOT)"/libnginx-mod-http-cache-pilot_*_$$(dpkg --print-architecture).deb
	./debian/tests/smoke

debian-source-package-signed:
	@if [ -n "$${DEBIAN_GPG_PRIVATE_KEY:-$${LAUNCHPAD_GPG_PRIVATE_KEY:-}}" ]; then \
		printf '%s\n' "$${DEBIAN_GPG_PRIVATE_KEY:-$${LAUNCHPAD_GPG_PRIVATE_KEY:-}}" | gpg --batch --import; \
	fi
	@key_id="$${DEBIAN_GPG_KEY_ID:-$${LAUNCHPAD_GPG_KEY_ID:-}}"; \
	test -n "$$key_id" || { echo "DEBIAN_GPG_KEY_ID or LAUNCHPAD_GPG_KEY_ID is required"; exit 1; }
	$(MAKE) debian-source-package
	key_id="$${DEBIAN_GPG_KEY_ID:-$${LAUNCHPAD_GPG_KEY_ID:-}}"; \
	changes_file="$$(ls -1 "$(DEBIAN_BUILD_ROOT)"/*_source.changes | tail -n 1)"; \
	debsign -k"$$key_id" "$$changes_file"

launchpad-ppa-upload: debian-source-package-signed
	changes_file="$$(ls -1 "$(DEBIAN_BUILD_ROOT)"/*_source.changes | tail -n 1)"; \
	dput "$(LAUNCHPAD_PPA)" "$$changes_file"
