# ngx_cache_purge — testing guide

All test infrastructure lives here. The repo root contains only the nginx
module source (`ngx_cache_purge_module.c`, `config`) and `.github/`.

## Layout

```
t/
├── Dockerfile              # Test image (build context = repo root)
├── Makefile                # Local dev commands (run from t/)
├── docker-compose.test.yml # Compose services for each test suite
├── basic.t                 # Core purge functionality
├── background_queue.t      # Async queue behaviour
├── config.t                # Directive validation
├── memory.t                # Slab alloc / leak checks
└── performance.t           # Timing and throughput
```

## Quick start

```bash
cd t/

# Build + run full suite (default nginx 1.28.2)
make test-all

# Specific nginx version
NGINX_VERSION=1.29.6 make test-all

# Single suite
make build
docker run --rm -v "$PWD/..:/src" ngx-cache-purge-test:1.28.2 prove -v t/basic.t
```

## All make targets

| Target | Description |
|---|---|
| `make build` | Build `ngx-cache-purge-test:<version>` image |
| `make test` | basic.t + config.t only |
| `make test-all` | All five suites |
| `make test-compat` | Build + test across 1.20.2 / 1.26.3 / 1.28.2 / 1.29.6 |
| `make test-version VERSION=x.y.z` | One specific version |
| `make shell` | Interactive shell in the container |
| `make clean` | Remove cache dirs, servroot, containers |

Set `NGINX_VERSION=x.y.z` to override the default (1.28.2).

## docker-compose

From inside `t/`:

```bash
# Full suite
docker-compose -f docker-compose.test.yml run --rm nginx-test

# Individual suite
docker-compose -f docker-compose.test.yml run --rm test-basic
docker-compose -f docker-compose.test.yml run --rm test-queue
docker-compose -f docker-compose.test.yml run --rm test-config
docker-compose -f docker-compose.test.yml run --rm test-memory
docker-compose -f docker-compose.test.yml run --rm test-performance
```

Set the nginx version via environment variable:

```bash
NGINX_VERSION=1.29.6 docker-compose -f docker-compose.test.yml run --rm nginx-test
```

## Test design rules

Every test config must follow these rules to avoid nginx startup errors
on nginx ≥ 1.27:

1. **No `location /`** — use named prefixes (`/cache`, `/purge`, `/origin`,
   `/health`) so nginx never synthesises a duplicate root location.

2. **No `proxy_cache` + 3-arg `proxy_cache_purge` in the same block** — the
   module rejects this combination at config time. Use the 3-arg form only in
   a dedicated purge location; use the inline `PURGE from …` form in proxy
   locations.

3. **`upstream backend` port** — always interpolate `server_port()` into the
   upstream inside `qq{}` so the port matches the live test server.

4. **Cache file KEYs** — fake cache files created in `--- init` blocks must
   use `KEY: /purge/...` (or whatever prefix the location uses) so the
   partial-walk prefix matching hits them correctly.

## CI

GitHub Actions (`.github/workflows/ci.yml`) runs the full matrix
automatically on push / PR. `CHANGELOG.md` is generated from git commits
in the `create-release` job — no static `CHANGES` file is maintained.
