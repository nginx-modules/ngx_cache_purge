# t/Makefile — run from the t/ directory
# Build context for Docker is always the repo root (one level up).
.PHONY: help build test test-all test-compat test-version clean shell

NGINX_VERSION ?= 1.28.2
COMPOSE       := docker-compose -f docker-compose.test.yml
IMAGE         := ngx-cache-purge-test:$(NGINX_VERSION)

GREEN  := \033[0;32m
YELLOW := \033[1;33m
RED    := \033[0;31m
NC     := \033[0m

help:
	@echo "$(GREEN)ngx_cache_purge test commands$(NC) (run from t/)"
	@echo ""
	@echo "$(YELLOW)Targets:$(NC)"
	@echo "  build          Build Docker test image (NGINX_VERSION=$(NGINX_VERSION))"
	@echo "  test           Run basic + config tests"
	@echo "  test-all       Run full prove suite (all *.t files)"
	@echo "  test-compat    Build + test across all supported nginx versions"
	@echo "  test-version   Run test-all for a specific version (VERSION=x.y.z)"
	@echo "  clean          Remove cache dirs, servroot, containers"
	@echo "  shell          Interactive shell inside the test container"
	@echo ""
	@echo "$(YELLOW)Variables:$(NC)"
	@echo "  NGINX_VERSION  nginx version to build/test  (default: $(NGINX_VERSION))"
	@echo ""
	@echo "$(YELLOW)Examples:$(NC)"
	@echo "  make build"
	@echo "  make test"
	@echo "  make test-all"
	@echo "  NGINX_VERSION=1.29.6 make build test-all"
	@echo "  make test-compat"
	@echo "  make test-version VERSION=1.26.3"

build:
	@echo "$(GREEN)Building $(IMAGE)...$(NC)"
	docker build \
	    --build-arg NGINX_VERSION=$(NGINX_VERSION) \
	    --build-arg UBUNTU_VERSION=22.04 \
	    -f Dockerfile \
	    -t $(IMAGE) \
	    ..
	@echo "$(GREEN)Build complete$(NC)"

test: build
	@echo "$(GREEN)Running basic + config tests ($(IMAGE))...$(NC)"
	docker run --rm -v "$(shell pwd)/..:/src" $(IMAGE) \
	    prove -v t/basic.t t/config.t
	@echo "$(GREEN)Done$(NC)"

test-all: build
	@echo "$(GREEN)Running full test suite ($(IMAGE))...$(NC)"
	docker run --rm -v "$(shell pwd)/..:/src" $(IMAGE) \
	    prove -v t/basic.t t/background_queue.t t/config.t t/memory.t t/performance.t
	@echo "$(GREEN)Done$(NC)"

test-compat:
	@echo "$(GREEN)Compatibility test across supported nginx versions...$(NC)"
	@for v in 1.20.2 1.26.3 1.28.2 1.29.6; do \
	    echo "$(YELLOW)--- nginx $$v ---$(NC)"; \
	    $(MAKE) --no-print-directory NGINX_VERSION=$$v build test-all || exit 1; \
	done
	@echo "$(GREEN)All versions passed$(NC)"

test-version:
	@if [ -z "$(VERSION)" ]; then \
	    echo "$(RED)Usage: make test-version VERSION=x.y.z$(NC)"; exit 1; \
	fi
	$(MAKE) --no-print-directory NGINX_VERSION=$(VERSION) build test-all

clean:
	@echo "$(GREEN)Cleaning test artifacts...$(NC)"
	$(COMPOSE) down -v --remove-orphans 2>/dev/null || true
	rm -rf servroot cache cache2 perfcache bgcache concurrent throttle
	@echo "$(GREEN)Clean complete$(NC)"

shell: build
	docker run --rm -it -v "$(shell pwd)/..:/src" $(IMAGE) bash
