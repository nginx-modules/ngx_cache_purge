# Run from t/ or use the forwarding Makefile at the repository root.
NGINX_VERSION ?= 1.31.6
UBUNTU_VERSION ?= 24.04
COMPOSE ?= $(shell docker compose version >/dev/null 2>&1 && echo 'docker compose' || echo 'docker-compose')
COMPOSE_CMD = $(COMPOSE) -f docker-compose.test.yml
export NGINX_VERSION UBUNTU_VERSION

SUITES = test-basic test-queue test-config test-memory test-performance test-native test-runtime test-valgrind
.PHONY: help build test test-all $(SUITES) test-verify test-sanitize test-compat test-version shell clean

help:
	@echo "Container test commands (NGINX_VERSION=$(NGINX_VERSION))"
	@echo "  build          Build the integration test image"
	@echo "  test           Run basic and configuration tests"
	@echo "  test-all       Run every t/*.t suite"
	@echo "  $(SUITES)"
	@echo "  test-verify    Strict builds plus native and runtime tests"
	@echo "  test-sanitize  ASan/UBSan, threads, 16 workers, 16384 files"
	@echo "  test-compat    Full suite on 1.20.2, 1.26.3, 1.28.3, 1.30.5, 1.31.6"
	@echo "  test-version VERSION=x.y.z"
	@echo "  shell          Open the integration container"
	@echo "  clean          Remove test containers and generated test files"

build:
	$(COMPOSE_CMD) build nginx-test

test: build
	$(COMPOSE_CMD) run --rm nginx-test sh t/run_tests.sh t/basic.t t/config.t

test-all: build
	$(COMPOSE_CMD) run --rm nginx-test

$(SUITES): build
	$(COMPOSE_CMD) run --rm $@

test-verify test-sanitize:
	$(COMPOSE_CMD) build $@
	$(COMPOSE_CMD) run --rm $@

test-compat:
	@for version in 1.20.2 1.26.3 1.28.3 1.30.5 1.31.6; do \
	    ubuntu=$(UBUNTU_VERSION); \
	    case $$version in 1.20.2) ubuntu=20.04 ;; esac; \
	    $(MAKE) --no-print-directory NGINX_VERSION=$$version UBUNTU_VERSION=$$ubuntu test-all || exit 1; \
	done

test-version:
	@test -n "$(VERSION)" || { echo "Usage: make test-version VERSION=x.y.z"; exit 1; }
	$(MAKE) --no-print-directory NGINX_VERSION=$(VERSION) test-all

shell: build
	$(COMPOSE_CMD) run --rm nginx-test bash

clean:
	$(COMPOSE_CMD) down --volumes --remove-orphans
	rm -rf test-results servroot ../cache ../cache2 ../perfcache ../bgcache ../concurrent ../throttle
