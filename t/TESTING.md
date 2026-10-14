# Testing ngx_cache_purge

Local Linux tests and Linux CI run inside Docker. The repository-root
Makefile forwards to `t/Makefile`; both use `t/docker-compose.test.yml`.
The image contains NGINX, its configured sources, compilers and Perl test
dependencies. A host NGINX or Perl installation is not required.

## Quick start

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
make test-version VERSION=1.26.3
```

The default is NGINX 1.31.6 on Ubuntu 24.04. `test-compat` uses Ubuntu 20.04
for the NGINX 1.20.2 integration build, matching CI; individual runs of that
version should set `UBUNTU_VERSION=20.04`. Every test target builds its image
before running. When using Compose directly, rebuild after changing
NGINX_VERSION or module sources; a bind mount does not rebuild `nginx`.

From `t/`:

```sh
docker-compose -f docker-compose.test.yml build nginx-test

# All seven TAP suites.
docker-compose -f docker-compose.test.yml run --rm nginx-test

# Individual suites.
docker-compose -f docker-compose.test.yml run --rm test-basic
docker-compose -f docker-compose.test.yml run --rm test-queue
docker-compose -f docker-compose.test.yml run --rm test-config
docker-compose -f docker-compose.test.yml run --rm test-memory
docker-compose -f docker-compose.test.yml run --rm test-performance
docker-compose -f docker-compose.test.yml run --rm test-native
docker-compose -f docker-compose.test.yml run --rm test-runtime
```

With the Compose plugin, substitute `docker compose` for `docker-compose`.
The Makefiles detect both; `COMPOSE="docker compose"` overrides detection.
Use `run`, as above, to select a service. `up` would start independent test
services concurrently against the same mounted work directory.

## Suites and entry points

| Compose service / Make target | Test file | What it checks |
|---|---|---|
| `test-basic` | `basic.t` | Exact/wildcard HTTP behavior, access, response formats and inherited configuration |
| `test-queue` | `background_queue.t` | Queue directives and request acceptance |
| `test-config` | `config.t` | Directive contexts, parsing, defaults and validation |
| `test-memory` | `memory.t` | Repeated purge submission and allocation workload smoke tests |
| `test-performance` | `performance.t` | Directory-walk and request-load smoke tests, not a latency benchmark |
| `test-native` | `native_helpers.t` | C helper boundaries, scheduler state, cleanup, diagnostics, access matching and structure layouts |
| `test-runtime` | `purge_runtime.t` | Real cache files, HTTP/status, traversal, batching, admission, metrics, reload and shutdown |

`nginx-test` runs `sh t/run_tests.sh`, which invokes `prove -v t/` and
therefore discovers every top-level `.t` file. Individual services pass
one filename to that same wrapper. There is no separate CI suite list.
The wrapper preserves `prove`'s failure status, including when producing
JUnit XML; CI does not rerun tests to generate reports.

The five original suites use `Test::Nginx::Socket`. The native/runtime suites
use core Perl modules and C fixtures. `NGINX_SOURCE` is required for native
layout checks and is set by the image. `TEST_NGINX_THREADS=1` in the integration
image makes the runtime suite select the named `cleanup` pool; builds without
threads exercise incremental execution. The implicit pool is `default`.
Set `TEST_NGINX_WORKERS` and `TEST_NGINX_FILES` with Compose
`run -e` to override the defaults of 8 workers and 4096 fixture files.

## Build, instrumentation and reports

| Make target | Container operation |
|---|---|
| `build` | Build the integration image |
| `test` | Basic and configuration suites |
| `test-all` | All seven TAP suites |
| `test-verify` | Strict debug/release module builds, then native and runtime suites |
| `test-sanitize` | ASan/UBSan with threads, 16 workers and 16384 files |
| `test-valgrind` | Memory suite with Test::Nginx's Valgrind launcher enabled |
| `test-compat` | All seven suites on 1.20.2, 1.26.3, 1.28.3, 1.30.5 and 1.31.6 |
| `test-version VERSION=x.y.z` | All seven suites on the selected version |
| `shell` | Open the integration container |
| `clean` | Remove project test containers and generated test files |

```sh
CC=clang NGINX_VERSION=1.31.6 make test-verify
NGINX_VERSION=1.31.6 make test-sanitize
NGINX_VERSION=1.31.6 make test-valgrind

# Save XML from the same run under t/test-results/.
TEST_REPORT_DIR=/reports make test-all
```

For custom strict-build options, from `t/`:

```sh
docker-compose -f docker-compose.test.yml build test-verify
docker-compose -f docker-compose.test.yml run --rm test-verify \
    perl t/build_verify.pl --nginx-source /opt/nginx --threads --workers 8
```

The `verify` image provides sources and dependencies; `build_verify.pl`
configures and builds inside the container. It compiles the module as GNU89
with `-Wall -Wextra -Werror -Wdeclaration-after-statement`, with debug on and
off, and with cache or all four protocol integrations disabled. It then runs
`native_helpers.t` and `purge_runtime.t`. The minimal build
disables rewrite and basic auth; integration images include rewrite and SSL.

The Valgrind service sets `TEST_NGINX_USE_VALGRIND` and
`TEST_NGINX_VALGRIND_EXIT_ON_FIRST_ERR`; merely installing Valgrind does not
instrument a server. Memory/performance smoke suites do not establish bounded
memory usage or a throughput SLA. Runtime fixtures exercise actual cache-file
deletion; sanitizer and Valgrind results apply to the paths exercised.

## Native helper and runtime coverage

Native fixtures compile extracted production helper bodies. Checks cover:

- Guard-page path boundaries, separator/case models and 32-bit clock rollover.
- Directory iteration/time budgets, ancestor/sibling paths after buffer growth,
  cleanup at every allocation failure with open parent directories, and read
  errors that must not be reported as a completed scan.
- PR72 descendant links and configured root links; deterministic directory
  replacement between classification and descriptor-relative opening.
- Pending/running key hash collisions, per-cache indexes, duplicate admission
  at capacity, FIFO replay, allocation failure and abandoned-owner recovery.
- Thread-pool submission failures, including condition-signalling failure,
  retained admission credits and successful resubmission without worker I/O.
- Three delayed cleanup retries, exhausted-attempt accounting and progress of
  another key while a failed task is waiting for its retry time.
- Native header/offset/version/CRC/hash validation, inclusive ETag/Vary limits,
  short reads and descriptor cleanup; fixtures use the configured core's ABI.
- Generation exact/prefix/baseline rules, memory-pressure invalidation, bounded
  garbage reclamation, counter saturation and conditional invalidation races;
  empty, single-byte and maximum-length selectors, plus over-limit rejection.
- A cache-manager callback prototype preserving both manager and loader data,
  including the seconds/milliseconds API boundary at NGINX 1.11.5. This is a
  prototype test, not a production manager backend or a performance comparison.
- Aligned per-worker counters, lock-free snapshots and 64-bit byte totals.
- Batch accounting across continuations, lock retries and thread completions.
- Exit cleanup while a core thread still owns traversal state.
- File-error diagnostics, initialized read metadata and descriptor closing.
- IPv4-mapped IPv6 access matching with high-bit addresses.
- Private protocol layout offsets and native cache-rbtree key representation.

The real-server suite covers status types and scopes, cache-path validation,
matching and filename guards, configured temporary paths, symlinks, queue
capacity, duplicate coalescing, bare millisecond values and malformed access
lists. Responses are checked for escaping and content length in all four body
formats, both after enqueueing and after synchronous completion. The suite
also checks default JSON, bodyless mode, binary-key Base64, problem details,
header/body code agreement, method Allow headers and ignored legacy flags.

Generation tests cover logical completion before deferred unlink, reload,
persistent restart epochs and a controlled late writer. REFRESH tests cover
304/200/404/410, origin and transport failures, escaped URI/query identity,
literal Host validation, deadline results, a late probe racing with PURGE,
Vary rejection and continuation beyond a 32-entry page. A forked local origin
uses explicit handshake files to order races. Linux runs it only in Docker;
native macOS runs it in its dedicated platform job. Windows HTTP tests also
cover generation invalidation, REFRESH and namespace reuse on reload.

All production C units listed in `config` are compiled under strict
debug/release and disabled cache/proxy/protocol guards. The strict matrix
checks 1.7.9, the epoll fallback on 1.7.11, corrected thread notification on
1.7.12, and current mainline. Extended runs add 1.11.4/1.11.5 manager API
boundaries. See [components](../docs/ARCHITECTURE.md) for the fallback reason. Test definitions are coverage
intent, not evidence of a successful run. No portable latency SLA follows
from these tests. Multi-stream HTTP/2/3 disconnect/pressure, long-running
slow-storage latency, RSS/FD ceilings and abrupt shared-memory crash recovery
remain deployment qualification work.

Lifecycle tests observe an active walk while one worker serves HTTP, then
exercise reload, graceful shutdown and worker termination. Full-queue admission waiting
must yield while its HTTP request remains open. Reload ownership tests pause
only the test instance's old workers, observe pending work in a new generation,
and check the retained protection settings. These tests disable `accept_mutex`
so a paused worker cannot retain the accept lock.

Task batches of two must leave the third task pending during the throttle
pause in both execution modes. This verifies the public batch contract;
a native fixture separately verifies scheduler state across callbacks.

### Logging contracts

Severity and formatting follow the NGINX [development guide](https://nginx.org/en/docs/dev/development_guide.html#logging)
and the corresponding cache, filesystem and access-module operations in core.

Direct module diagnostics use the `ngx_cache_purge: ` message prefix at every
level, including configuration and debug messages. Module-owned configuration
errors are logged with `ngx_conf_log_error()` and return `NGX_CONF_ERROR`, so
the parser does not print a second copy. Diagnostics from core helpers and
core directive setters retain their native format. NGINX adds errno and
request/configuration context; the module does not replace its log writer.

Filesystem messages use the actual failing operation, a quoted path and the
saved errno. Descriptor-relative failures name `openat()`, `fdopendir()`,
`fstatat()` or `unlinkat()`; fallback and Windows operations use core names.
Debug macros use `NGX_LOG_DEBUG_HTTP`; the suffix in `ngx_log_debug2` means
two format arguments, not a different verbosity level.

| Event | Level | Detail checked |
|---|---|---|
| Removed pool on an accepted task | warn | Incremental fallback for the old task |
| Access-list denial | error | Client address, matching the core access-module severity |
| Queue capacity reached | metrics | Waiting and overflow count; no per-retry warning |
| Task allocation failure | metrics | Admission waiting without a request-bound file walk |
| Path metadata allocation failure | emerg | Configuration rejected before accepting tasks |
| Reload metadata allocation failure | emerg | Zone and capacity |
| Queue resizing | emerg | Capacity or zone-size change and restart requirement |
| Namespace rejected | emerg | Namespace name, actual/required size, state version or old/new path |
| Excessive wildcard prefix | warn | Length/limit, once per request |
| File open failure | crit | Actual operation, path and errno, matching core cache I/O |
| File read failure | crit (POSIX), error (Windows) | Core I/O diagnostic with initialized filename |
| File close failure | alert | Operation, path and errno |
| File deletion failure | crit | Actual operation, path and errno |
| Three cleanup retries exhausted | error | Cache path; task retired, operator can retry PURGE |
| REFRESH profile rejected | error | Profile URI and specific rejected setting or header name; header values are not logged |
| File already absent | none | No module diagnostic for an expected race |

Runtime tests check request-level messages. Native fixtures inject file errors
and allocation failure at the queue's slab API boundary; they do not exhaust
the real shared zone. Every managed test server is checked for unexpected severe
(`crit`, `alert`, `emerg`) diagnostics and sanitizer reports. Expected resize
rejections and core cache-file `stat()` races are filtered narrowly.

Sanitizer builds link `fixtures/nginx_lsan.c`, which supplies an LSan exception
for the exact `ngx_event_process_init` function. NGINX 1.31.6 leaves its
connection/read/write-event arrays allocated at worker exit; after a reload,
TERM can make these three core allocations unreachable. This is the
[upstream-reported shutdown cleanup issue](https://mailman.nginx.org/pipermail/nginx-devel/2024-February/V5O33KADRTB63SXHS4IWCGIRIIYGN23H.html).
The exception applies to allocation stacks containing that function, not to
`ngx_alloc`, worker initialization in general, or purge functions. Leak detection
stays enabled. A sanitizer-only native regression checks that an unsuppressed
control leak fails, the known allocation path is excepted, and a separate purge
allocation still produces a fatal leak report. Native helper tests themselves
do not inherit the NGINX exception. The core source is not patched.

Linux sanitizer builds also link `fixtures/nginx_lsan_cycle.c` with GNU
`--wrap=ngx_init_cycle`. A successful `nginx -t` in NGINX 1.31.6 returns from
`main()` before storing its newly initialized cycle in the global `ngx_cycle`.
The wrapper keeps only that successful test cycle reachable until process exit.
It does not retain normal server cycles or failed configuration cycles, and does
not suppress allocations made by `ngx_init_cycle` or module configuration
callbacks. A separate LSan regression verifies the raw unrooted cycle is
reported, the test cycle is retained, and both an unrelated configuration leak
and an unrooted runtime cycle still fail. This wrapper is absent from production,
integration, Windows and non-sanitizer builds.

Every runtime-suite `nginx -t` invocation checks for sanitizer reports, including
intentionally invalid configurations. A sanitizer failure cannot count as a
successful rejection. Failed positive configuration assertions print the full
captured process output into the TAP/CI log and the accompanying stderr artifact.

### Restricted environments

If a root-run container blocks `initgroups(root, 0)`, the explicit option
`--allow-initgroups-warning` permits only that core diagnostic. It does not
change worker credentials; CI leaves the exception disabled.

When LeakSanitizer cannot inspect processes, `--sanitize --no-leak-check`
links a test-only LSan opt-out into the server and configure probes. ASan and
UBSan remain enabled; this run does not verify leaks. The same explicit option disables leak inspection in instrumented native
fixtures; guard-page fixtures always disable it. CI leaves the option off.
Sanitizer builds use the core portable MD5 implementation
(`NGX_HAVE_NONALIGNED=0`) to avoid unaligned typed loads while retaining
UBSan alignment checks. These are test options, not module directives.

## Platform jobs and CI

All jobs live in `.github/workflows/ci.yml`:

| Job | Environment | Checks |
|---|---|---|
| `test-matrix` | Docker, Ubuntu 20.04/24.04 | Every TAP suite; the README lists NGINX versions |
| `build-compatibility` | Docker, Ubuntu 24.04 | GCC/Clang, strict debug/release compilation, native and runtime suites, selected thread builds |
| `sanitizer` | Docker, NGINX 1.31.6 | ASan/UBSan, pooled and incremental modes, 16 workers/16384 files |
| `memory-leak-test` | Docker, NGINX 1.31.6 | Valgrind-instrumented memory suite, extended runs |
| `native-macos` | macOS 14, NGINX 1.31.6 | Clang build, native helpers and POSIX runtime suite |
| `native-windows` | Windows 2022, MSVC x64/x86, NGINX 1.31.6 | Native build and `windows_runtime.ps1` HTTP/filesystem/lifecycle checks |
| `static-analysis` | Docker | Cppcheck |
| `semgrep-analysis` | Container | C/security/secrets rules |

`t/ci_matrix.pl` selects representative releases instead of a Cartesian
product of every minor version and compiler. Normal code changes run two
integration targets (stable/mainline), four strict boundary targets, sanitizer,
platform and analysis jobs. GCC/Clang and execution modes are grouped within
the mainline strict job, with separate report directories. Extended runs add
three older integration releases, two manager boundaries and Valgrind.

Push and pull-request branches remain `main`, `releases/**` and `dev/**`.
Extended checks run weekly, with `workflow_dispatch` input `full`, on release
branches and on `v4.*` tags. Documentation-only changes skip builds; an unknown
comparison base conservatively runs them. The stable `CI` check summarizes
all selected jobs and fails on failures, cancellations or unexpected skips.
Use that check for branch protection. The release job requires it to succeed.
Matrix groups allow at most two simultaneous jobs; a new run cancels an older
run for the same PR/ref, except tags. Separate groups can still run concurrently.

The local APT composite action pins `vegardit/fast-apt-mirror.sh` 1.4.5 by full
commit SHA. It selects a runner mirror, then passes the validated URL and CA
bundle through BuildKit secrets so Ubuntu containers use the selected archive
mirror too. The exporter follows the runner's `mirror+file:` source to its local
mirror list (including `/etc/apt/apt-mirrors.txt`), as well as direct Ubuntu
sources. If no eligible mirror exists, it omits the BuildKit secret completely
and the container keeps its default; it never sends an empty `apt_mirror=` value.
Mirror-list, direct-source and empty-fallback cases run in the selection job.
Security sources remain unchanged. The mirror URL does not vary
Docker cache keys; local builds without secrets keep distribution defaults.
APT and source downloads have bounded retries/timeouts. There is no DNS
probe or resolver reconfiguration.

JUnit XML and stdout/stderr come from the same test invocation. If the TAP
formatter bails out or produces no complete XML, a failed harness report is
written and the original command still fails. Report artifacts are kept on
failure. Fork pull requests upload artifacts without requesting a write-only
check publication token. A TERM-cleanup timeout includes server logs in the
TAP diagnostics instead of reporting only the timeout text.

Cppcheck consumes a compilation database for the configured NGINX tree and
all module units. All enabled findings remain visible. Four style suggestions
(`variableScope`, `constParameterPointer`, `constVariablePointer`,
`redundantContinue`) and unused inline functions in upstream headers are
advisory; upstream paths are recognized in both absolute and relative form.
Other findings fail the job. Strict compiler warnings remain errors.

Native platform jobs are the deliberate exception to Docker execution:
Linux containers on a Windows or macOS host still exercise Linux APIs.
The Windows job builds with MSVC through MSYS2, following the
[NGINX Windows build procedure](https://nginx.org/en/docs/howto_build_on_win32.html).
It checks MISS/HIT, exact and queued wildcard/full purges, access denial,
JSON/XML escaping, normalized status paths, temporary-directory protection,
junction exclusion, counters, reload and graceful shutdown. It does not
claim the POSIX signal/guard-page suite or thread-pool coverage.

To reproduce the Windows runtime job after building the module with MSVC,
run in PowerShell 7 on Windows:

```powershell
./t/windows_runtime.ps1 -Nginx ./nginx-1.31.6/objs/nginx.exe
```

Every runtime job uploads JUnit from its actual execution. Windows additionally
retains its transcript, configuration and server log. Build/test failures
remain job failures; report publication does not replace those gates. Release
creation depends on the build, runtime, sanitizer, platform and analysis jobs.
The release job generates its changelog from commits.

A configured matrix entry is not evidence of a passing run. Record the source
version, compiler, flags, load and environment exceptions with each result.
Native BSD, interprocess crash recovery, shared-slab fault injection and
sustained storage-latency measurements remain separate qualification work.

## Test design

Use named locations such as `/cache`, `/origin` and `/purge` to avoid colliding
with a harness-provided root location. Use zone/key purge syntax in a dedicated
location without the corresponding cache/pass directive. Interpolate
`server_port()` for loopback backends. Files intended for deletion must have a
native cache header, `\nKEY: ` marker, matching key and a 32-hex-digit filename.
Legacy smoke fixtures with arbitrary filenames check walking/acceptance;
they do not prove that cache entries were deleted.

The queue helper checks 10,000 pending and 10,000 active duplicate admissions,
one replay allocation, full-capacity waiting, rejected pool submissions (including
condition-signal failure), process ownership recovery, graceful/fast-exit accounting,
alignment and byte counters above 32 bits. Status is an approximate snapshot and its
fields must not be tested as an atomic cross-worker transaction. Named pool validation
and log-phase HIT/MISS observation are included in `purge_runtime.t`.
