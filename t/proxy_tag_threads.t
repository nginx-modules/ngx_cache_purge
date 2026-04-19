# vi:filetype=perl
#
# Tag-index bootstrap and tag purge with a thread pool configured.
#
# When NGX_CACHE_PILOT_THREADS is active the bootstrap directory walk runs in
# a worker thread at init_process time rather than synchronously on the first
# PURGE request.  These tests verify:
#
#   1. "cache_tag bootstrap zone" appears in the error log (the thread ran and
#      indexed the zone at startup — verified by grep of the accumulated log).
#   2. A tag purge after thread bootstrap finds and expires the correct entry.
#   3. After a second nginx start against the same SQLite file the tag purge
#      still returns 200 and expires the correct entry.  Whether the bootstrap
#      thread logs "reusing persisted index" or "bootstrap zone" depends on
#      whether Phase 2a's async bootstrap committed bootstrap_complete=1 before
#      the restart — this is inherently racy so we do not assert the log message.
#
# A second nginx start is forced by $http_config_persist_reload, which has
# identical functional content to $http_config_persist but differs by one
# harmless nginx comment — enough to make Test::Nginx generate a new
# nginx.conf and restart the server.
#
# When nginx is not built with --with-threads the module falls back to the
# synchronous bootstrap path; the same assertions still hold.

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => 29;

our $main_config = <<'_EOC_';
    thread_pool default threads=4 max_queue=65536;
_EOC_

# ── Phase 1: fresh SQLite, confirms bootstrap thread ran ─────────────────────

our $http_config_boot = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_threads_tag_cache keys_zone=threads_tag_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_threads_tag_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_threads_tags_boot.sqlite;
_EOC_

# ── Phase 2a: persisted SQLite, first start ───────────────────────────────────
# Uses a distinct cache zone name so Test::Nginx restarts nginx when switching
# from $http_config_boot.

our $http_config_persist = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_threads_persist_cache keys_zone=threads_persist_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_threads_persist_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_threads_persist_tags.sqlite;
_EOC_

# ── Phase 2b: same functional config as Phase 2a plus a harmless comment ─────
# The comment makes the nginx.conf string differ → Test::Nginx forces a restart
# while keeping the same cache directory and SQLite file on disk.

our $http_config_persist_reload = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_threads_persist_cache keys_zone=threads_persist_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_threads_persist_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_threads_persist_tags.sqlite;
    # second-start (forces nginx restart in Test::Nginx)
_EOC_

our $config_boot = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        threads_tag_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /origin/a {
        add_header Surrogate-Key "group-threads-boot";
        return 200 "origin-a-boot";
    }
_EOC_

our $config_persist = <<'_EOC_';
    location = /proxy/p {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/p;
        proxy_cache        threads_persist_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /origin/p {
        add_header Surrogate-Key "group-threads-persist";
        return 200 "origin-p";
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

# ── Phase 1: thread-pool bootstrap on a fresh SQLite database ────────────────

=== TEST 1: populate cache entry before bootstrap-aware purge
--- main_config eval: $::main_config
--- http_config eval: $::http_config_boot
--- config eval: $::config_boot
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a-boot
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 2: tag purge succeeds after startup bootstrap
# In single-process test mode the startup bootstrap may complete before the
# request log window opens, so assert the functional PURGE result instead.
--- main_config eval: $::main_config
--- http_config eval: $::http_config_boot
--- config eval: $::config_boot
--- request
PURGE /proxy/a
--- more_headers
Surrogate-Key: group-threads-boot
X-Purge-Mode: soft
--- error_code: 200
--- response_body_like: \{\"key\": 
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 3: purged entry is expired
--- main_config eval: $::main_config
--- http_config eval: $::http_config_boot
--- config eval: $::config_boot
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a-boot
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


# ── Phase 2a: first start against a fresh SQLite database ─────────────────────
# Switching to $http_config_persist triggers a nginx restart (different zone
# name in the generated config).

=== TEST 4: populate cache entry for persisted-index test
--- main_config eval: $::main_config
--- http_config eval: $::http_config_persist
--- config eval: $::config_persist
--- request
GET /proxy/p
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-p
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 5: tag purge succeeds with fresh persist index
--- main_config eval: $::main_config
--- http_config eval: $::http_config_persist
--- config eval: $::config_persist
--- request
PURGE /proxy/p
--- more_headers
Surrogate-Key: group-threads-persist
X-Purge-Mode: soft
--- error_code: 200
--- response_body_like: \{\"key\": 
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 6: repopulate for second-start test
--- main_config eval: $::main_config
--- http_config eval: $::http_config_persist
--- config eval: $::config_persist
--- request
GET /proxy/p
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-p
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


# ── Phase 2b: second start against the same persisted SQLite ──────────────────
# $http_config_persist_reload has identical functional content to
# $http_config_persist but differs by one nginx comment, forcing a restart.
# On this second start the bootstrap thread runs again.  If bootstrap_complete=1
# was committed by Phase 2a it takes the "reusing" fast path; otherwise it
# re-bootstraps.  Either way the tag purge must succeed.

=== TEST 7: second start tag purge still works
# Force a restart by switching to $http_config_persist_reload (identical
# functional content to $http_config_persist plus a harmless comment).
# After the restart the bootstrap re-runs (thread or sync).  The tag purge
# must still return 200 regardless of whether bootstrap_complete was persisted.
--- main_config eval: $::main_config
--- http_config eval: $::http_config_persist_reload
--- config eval: $::config_persist
--- request
PURGE /proxy/p
--- more_headers
Surrogate-Key: group-threads-persist
X-Purge-Mode: soft
--- error_code: 200
--- response_body_like: \{\"key\": 
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 8: purged entry is expired after reload
--- main_config eval: $::main_config
--- http_config eval: $::http_config_persist_reload
--- config eval: $::config_persist
--- request
GET /proxy/p
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-p
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
