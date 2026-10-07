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

## Purge safety and queue status

The focused workflow `purge-status.yml` builds NGINX 1.7.9, 1.20.2, and
1.29.6 with GCC and Clang. It compiles this module separately with GNU89,
`-Wall -Wextra -Werror -Wdeclaration-after-statement`. Old NGINX core
warnings are not treated as errors; the module's strict check has no such
exception. A configured CI matrix is not evidence that its jobs passed.

Run the focused checks from the module directory:

```sh
CC=gcc CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
    python3 t/purge_bounds.py
python3 t/purge_status.py /absolute/path/to/nginx
PURGE_TEST_WORKERS=8 PURGE_TEST_FILES=8192 \
    python3 t/purge_status.py /absolute/path/to/sanitized/nginx
```

The boundary test compiles the actual guards and timeout expression with
a small type adapter. It covers exact-length nonterminated buffers, root
boundaries, reserved-name prefixes, both separator conventions, filename
case handling, clock skew, and 32-bit rollover. It models Windows path
semantics; it does not execute Windows filesystem operations.

The integration test starts real workers and checks JSON/HEAD/method handling,
path isolation, shared-key purge modes, duplicates at capacity, synchronous
fallback, unknown paths, overlapping walks, Vary, failed resizing reloads,
and draining after enqueueing is disabled. It verifies filesystem results
as well as status. Runtime coverage uses proxy caching; other protocols
still need native integration coverage. Run native FreeBSD, macOS, and
Windows checks before claiming those platforms validated.

The queue invariant is that each linked item contributes one waiting count
and, for purge-all, one pending count to its independently owned path record.
Dequeue removes those contributions and adds one active count under the same
mutex. Completion removes the active count before freeing the item. Status
reads only these counters and the global capacity under that mutex.
No queue/slab mutex is held while walking or while formatting JSON.

Constant work under a mutex does not guarantee sub-millisecond latency.
Sanitizers do not prove crash recovery or validate every shared-slab lifetime.
Timers use NGINX's signed modular difference convention, requiring relevant
intervals below half the millisecond counter range. Queue idle does not prove
that every accepted purge succeeded; inspect filesystem effects and logs.

Local validation on Linux with GCC 13.3.0 passed strict module compilation
and real-worker tests on all three versions above. NGINX 1.29.6 also passed
the integration test with ASan/UBSan, eight workers, and 8192 fixture files.
Leak detection was disabled because the sandbox does not expose procfs;
this is not a leak-check result. The origin supplies ETag headers to avoid
the core cache writer's zero-length NULL memcpy diagnostic under UBSan.
The bounds test passed ASan/UBSan in all three modeled path configurations.
Clang jobs and native non-Linux runs have not been executed locally.
