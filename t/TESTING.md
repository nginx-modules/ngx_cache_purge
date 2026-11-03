# Testing ngx_cache_purge

Local Linux tests and Linux CI run inside Docker. The repository-root
Makefile forwards to `t/Makefile`; both use `t/docker-compose.test.yml`.
The image contains NGINX, its configured sources, compilers and Perl test
dependencies. A host NGINX or Perl installation is not required.

## Quick start

```sh
make test-all
NGINX_VERSION=1.31.7 make test-all
make test-version VERSION=1.26.3
```

The default is NGINX 1.31.7 on Ubuntu 24.04. `test-compat` uses Ubuntu 20.04
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
| `test-compat` | All seven suites on 1.20.2, 1.26.3, 1.28.3, 1.30.6 and 1.31.7 |
| `test-version VERSION=x.y.z` | All seven suites on the selected version |
| `shell` | Open the integration container |
| `clean` | Remove project test containers and generated test files |

```sh
CC=clang NGINX_VERSION=1.31.7 make test-verify
NGINX_VERSION=1.31.7 make test-sanitize
NGINX_VERSION=1.31.7 make test-valgrind

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

For a feature-by-feature map, including missing end-to-end cases, see
[what the v4 tests cover](COVERAGE.md). The suite has no measured
line/branch coverage percentage; successful jobs do not imply complete coverage.

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
- Traffic class names bound to stable shared-zone slots: reorder, drop, re-add,
  refusal when no slot is free, and no slot consumed before `init_module`.
- Shared-zone layout fingerprint per `NGX_CACHE_PURGE_SHM_VERSION` (struct
  sizes and two metric indexes, not every member offset) and refusal of a zone
  of another version by `init_shm_zone()` before any version-dependent field is
  read or the zone is written.
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
profile isolation, deadline results, a late probe racing with PURGE,
Vary rejection and continuation beyond a 32-entry page. A forked local origin
uses explicit handshake files to order races. Linux runs it only in Docker;
native macOS runs it in its dedicated platform job. Windows HTTP tests also
cover generation invalidation, REFRESH and namespace reuse on reload.

Profile tests use a confirmed cache HIT before testing a probe. A separate
blocked-refill scenario checks that a completed empty scan does not stand in
for profile validation. Unsafe-profile cases check forwarding, cache and error
interception settings, method, internal location, Host, validator variables and
representation headers. Rejection must preserve the cached HIT and send no HEAD
to the controlled origin. These cases use independent servers rather than a
reload shared with an earlier invalidation test.

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

## Compatibility matrix

The historical 1.7.x and 1.11.x targets exercise core API and lifecycle
boundaries. They are not recommended deployment releases and do not imply that a
maintained distribution ships them. The matrix pins exact upstream source
versions; it does not qualify every vendor-patched package or ABI. For
deployments, follow the distribution's package maintenance policy or NGINX's
current stable/mainline releases.

The matrix lists configured CI targets. A successful job qualifies that source
and build configuration; it does not cover every release in a series.

| NGINX | Commit checks | Extended checks |
|---|---|---|
| 1.30.6, 1.31.7 | All seven container suites | Same coverage |
| 1.31.7 | Strict GCC/Clang, incremental/pooled; ASan/UBSan; native macOS and Windows x64/x86; Valgrind on PRs | Valgrind also outside PRs |
| 1.7.9 | Strict GCC, native helpers and runtime, incremental | Same coverage |
| 1.7.11, 1.7.12 | Strict GCC; epoll fallback / first corrected thread-pool notification | Same coverage |
| 1.20.2, 1.26.3, 1.28.3 | — | All seven container suites |
| 1.11.4, 1.11.5 | — | Strict GCC; manager callback seconds/milliseconds boundary |

Linux jobs use the same Docker images and test entry point as local runs. Native
macOS and Windows jobs exercise their own filesystem and process APIs; Windows
builds with MSVC and runs `nginx.exe` HTTP, path and reload checks. The POSIX
process/signal fixtures run on Linux and macOS. Windows does not run those
fixtures or the optional NGINX thread-pool path.

Thread support is optional. Builds without `--with-threads` use incremental
traversal; epoll builds before 1.7.12 also use that fallback to avoid a core
thread-notification race. BSD has no native CI job. Commit checks cover API
boundaries and current stable/mainline releases; scheduled, manual full and
release runs add historical representatives. Documentation-only changes skip
builds. The commands and triggers are described below.

## Platform jobs and CI

Build, runtime and release jobs live in `.github/workflows/build-test-release.yml`:

| Job | Environment | Checks |
|---|---|---|
| `test-matrix` | Docker, Ubuntu 20.04/24.04 | Every TAP suite; see the compatibility matrix above |
| `build-compatibility` | Docker, Ubuntu 24.04 | GCC/Clang, strict debug/release compilation, native and runtime suites, selected thread builds |
| `sanitizer` | Docker, NGINX 1.31.7 | ASan/UBSan, pooled and incremental modes, 16 workers/16384 files |
| `memory-leak-test` | Docker, NGINX 1.31.7 | Valgrind-instrumented memory suite, code PRs and extended runs |
| `native-macos` | macOS 14, NGINX 1.31.7 | Clang build, native helpers and POSIX runtime suite |
| `native-windows` | Windows 2022, MSVC x64/x86, NGINX 1.31.7 | Native build and `windows_runtime.ps1` HTTP/filesystem/lifecycle checks |
| `static-analysis` | Docker | Cppcheck |
| `semgrep-analysis` | Container | C/security/secrets rules |

`t/ci_matrix.pl` selects representative releases instead of a Cartesian
product of every minor version and compiler. Normal code changes run two
integration targets (stable/mainline), four strict boundary targets, sanitizer,
platform and analysis jobs. GCC/Clang and execution modes are grouped within
the mainline strict job, with separate report directories. Extended runs add
three older integration releases and two manager boundaries. Valgrind also
runs on code pull requests and is required by the final CI gate.

Push and pull-request branches remain `main`, `releases/**` and `dev/**`.
Extended checks run weekly, with `workflow_dispatch` input `full`, on release
branches and on `4.*` tags (for example `4.0.0`, without a `v` prefix).
Documentation-only changes skip builds; an unknown comparison base
conservatively runs them. The stable `CI` check summarizes
all selected jobs and fails on failures, cancellations or unexpected skips.
Use that check for branch protection. The release job requires it to succeed.
Matrix groups allow at most two simultaneous jobs; a new run cancels an older
run for the same PR/ref, except tags. Separate groups can still run concurrently.

BuildKit caches use the GitHub Actions backend with `mode=min` for integration,
verify and PVS-Studio. The manual benchmark uses `mode=max` to retain its build
stage after copying binaries into a smaller runtime image. Only workflows
running on `main` export BuildKit cache entries; pull requests, maintenance
branches and tag builds only restore eligible entries. A cache export failure
is nonfatal and its timeout is five minutes; compilation and test failures
still fail their jobs. Buildx's own binary cache is managed by its setup action.

Scopes are shared by target and NGINX/Ubuntu version: Valgrind imports the
1.31.7 integration image, and sanitizer/Cppcheck import the 1.31.7 verify image.
Integration and verify jobs also import the other mainline scope as a fallback
for common tool layers. The benchmark imports both scopes. BuildKit still
matches each layer's inputs, including the base distribution and core version;
an extra cache source does not substitute a different NGINX build. These
imports reuse available caches, without making parallel jobs wait for a writer.
Availability follows GitHub's branch-access rules: a cache from `main` is not
necessarily accessible if `main` is neither the default nor the PR base branch.

The Dockerfile installs explicit packages without APT recommendations and
removes CPAN build/download files in the installation layer. NGINX downloads
precede module sources; integration compilation precedes the complete test
checkout. Editing tests or documentation therefore preserves the server build
layer. Integration and verify retain compilers and configured sources because
the native fixtures compile against that ABI at test time. The benchmark
runtime contains only its binaries, layout/stack reports, Python and runtime
libraries. Its image build checks both binaries' linkage and Python imports.

Every Docker build records the loaded image's uncompressed size in the job
summary. BuildKit's build summary and logs provide cache-hit and timing evidence.
Compare equivalent runner, target and version combinations, including a cold
build and a repeated build. Image bytes are not compressed cache bytes or total
runner disk usage. Repository cache usage/evictions must be inspected separately
under Actions > Caches; no percentage saving is assumed from the Dockerfile.

JUnit and Docker build-record artifacts expire after 7 days, and intermediate
release-module artifacts after 3 days. Already compressed release archives use
artifact compression level zero. Published GitHub Release assets are independent
of these short-lived artifacts. GitHub normally evicts caches unused for seven
days and applies a 10 GB repository limit. Increasing storage beyond the free
allowance can incur charges; these workflows do not change that allowance.
Keep `mode=max` confined to the benchmark's separate, manually populated scope.

External Actions use full commit SHAs with release-version comments. Dependabot
checks them weekly and groups updates into one reviewable PR; it does not merge
them automatically. Checkouts do not persist Git credentials. Release steps
receive their existing explicit token and attestation permissions.

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
./t/windows_runtime.ps1 -Nginx ./nginx-1.31.7/objs/nginx.exe
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

### PVS-Studio

`.github/workflows/pvs-studio.yml` runs a separate PVS-Studio check on pushes
to `main`, `releases/**` and `dev/**`, weekly on the default branch, and through
manual dispatch on branches. Pull requests and tags do not run this check.
README, `docs/`, license and issue/PR-template-only branch changes skip it;
Scheduled runs and manual dispatch on branches still run the check. Add
**Actions secrets** `PVS_STUDIO_LICENSE_NAME` and
`PVS_STUDIO_LICENSE_KEY` in the repository or its `PVS-Studio` environment
using a valid PVS-Studio license. Missing credentials
fail the check with a setup message. Dependabot runs skip the job because they
cannot receive these secrets. Merged changes are analyzed on the branch push.

The `pvs-studio` stage in `t/Dockerfile` installs PVS-Studio from its signed APT
repository in a minimal Ubuntu image with GCC, Perl, zlib and tracing tools.
Its source entry keeps `arch` and `signed-by` in one APT options block;
the vendor's source list is not rewritten. An offline regression uses APT's
parser to validate the generated entry without installing the analyzer.
It omits the integration image's Clang, Valgrind and Perl test dependencies.
Module sources are not copied into the image, so edits do not invalidate the
analyzer installation or NGINX download layers. The `test-pvs` Compose service
configures the license and NGINX 1.31.7 with threads and debug enabled, then
traces the module's compiler commands inside Docker.
Only this service adds `SYS_PTRACE` for the build tracer. License credentials
are passed at runtime and are never included in image layers or build caches.
The checkout is mounted read-only; sources, build outputs and the license live
in the disposable container. Reports are written to `t/test-results/pvs-studio/`.

Rewrite and basic-auth modules are disabled, matching the Cppcheck
configuration; all four cache protocol integrations remain enabled. Analysis
excludes the NGINX source/build directory and retains this module's sources
and headers. `t/pvs_targets.pl` reads `CACHE_PURGE_SRCS` from `config` and finds
each module object in NGINX's generated Makefile. Only those objects are
rebuilt (currently seven); NGINX core compilation and linking are unnecessary
for this analysis. Missing or ambiguous targets fail the run. The build is
forced so cached objects cannot escape analysis, including after header edits.

The raw log and high/medium general-analysis (`GA:1,2`) task-list and SARIF
reports are retained for 7 days. Before generating SARIF, the converter writes
JSON with paths relative to `/src`, following the PVS-Studio GitHub Actions
guide; container paths therefore map to the checked-out repository. SARIF is
also uploaded to GitHub code scanning;
the repository must have code scanning available. Findings are advisory during
initial adoption, while installation, compilation, analysis, conversion and
upload failures fail the workflow. This separate check is not part of the
existing `CI` release gate. Review the first report before making it required
or introducing a findings baseline.

To run locally, export `LICENSE_NAME` and `LICENSE_KEY`
in your shell, then use the same container service as CI:

```sh
make test-pvs
```

No host PVS-Studio or NGINX installation is needed. Archive or remove previous
`t/test-results/pvs-studio/` reports before rerunning; the script requires an
empty directory to prevent stale results from being mistaken for a successful
retry. See the [PVS-Studio GitHub Actions guide](https://pvs-studio.com/en/docs/manual/6579/)
and [official Makefile tracing examples](https://github.com/viva64/pvs-studio-makefile-examples).

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

### Additional v4 regression cases

Native policy scenarios cover lock ownership, timeout/age, stale fallback,
minimum-use warmup and separate bypass/no-cache predicates for all four upstream
protocols. Proxy cases also hold revalidation and background updating across
PURGE. Real-file expiration uses the configured core header layout. The stale
case must first observe STALE; the background case observes STALE/UPDATING and
a late old-generation file after cleanup. These preconditions prevent a MISS
from accidentally satisfying a stale-path regression.

Vary cases populate 66 real representations across two header axes, preserve
case/prefix neighbors, repeat after reload and verify replacement bodies. A
held-body case lets headers pass before PURGE, drains cleanup, then verifies
late cache publication remains unreachable from the new generation.

The optional [performance workflow and source review](PERFORMANCE.md) describe
the complete native directive inventory, remaining scenarios, measurement
limits and memory tradeoffs. Performance runs are manual; normal test jobs do
not run the large cache populations or impose latency thresholds.

`purge_runtime.t` also starts small local FastCGI, SCGI and uWSGI origins from
`fixtures/gateway_origin.pl`. Each protocol fills a real NGINX cache and checks
exact/prefix PURGE with generation keys enabled and disabled. No external app
server is needed. REFRESH tests exercise malformed and duplicate cursors,
redirected profiles, shared namespaces across cache paths, scan errors,
client-header isolation, stored validators, origin errors and cancellation of
a page with an active HEAD. The queue fixture exercises the production shared
probe-credit function, including the 16-credit limit and abandoned owners.

The disconnect case enables info logging to observe NGINX's disconnect event.
The scan-fault case temporarily replaces its private cache root with a regular
file, then restores it; this reliably produces ENOTDIR even as root. Only that
expected path-specific diagnostic is exempted from the severe-log check.

The missing-ETag scenarios expose a separate upstream defect in NGINX 1.31.6:
`ngx_http_file_cache_set_header()` passes a NULL ETag pointer to `memcpy` with
length zero. Under UBSan, each affected scenario records a visible TODO failure
for that exact core source statement. The matcher derives its line from the
configured NGINX source, requires the specific argument-2 nonnull diagnostic,
and only applies in the two intentionally missing-ETag cases. All other core
and module sanitizer diagnostics still fail the suite. This is an unresolved
NGINX defect, not a clean UBSan result or a module fix. The behavioral assertions
for date-only and absent validators still run normally.

### Listener ownership in runtime fixtures

A controlled origin binds its listener before the test chooses the NGINX port.
Choosing and releasing the NGINX port first lets the origin immediately reuse
it. An HTTP 200 from that origin could then masquerade as a healthy NGINX while
the real master retries bind(). The regression forces this legal port-reuse
order with real sockets; it also requires a cache HIT. Readiness checks the
health body, and warmup failures include the last response and bounded
process/error-log tails. Timeouts and behavioral assertions are not relaxed.


## Release module checks

`tools/build-release-module.sh` compiles a dynamic module with `--with-compat`
and tests its loading with a purge configuration before packaging it. Release
jobs use native x86_64 and aarch64 runners for the pinned stable and mainline
versions. This load check does not replace the runtime or Valgrind suites.

`python3 -m unittest discover -s t -p test_release.py -v` checks publication
ordering and failure handling using a fake GitHub CLI, checks that differing
rebuild archives block publication, and exercises the final
CI gate for PR, push, full and documentation-only runs. It performs no GitHub
writes. The prepare job runs these tests on every CI invocation.

See [dynamic modules and release archives](../docs/BINARY_MODULES.md) for the
binary compatibility contract and immutable-release publishing sequence.
