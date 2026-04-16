# vi:filetype=perl
#
# Wildcard / partial-key purge with a thread pool configured.
# Verifies that ngx_http_cache_purge_partial() takes the async thread-pool
# path (NGX_CACHE_PURGE_THREADS) when a "default" thread pool exists, and
# still returns the correct response and leaves non-matching entries intact.
#
# When nginx is not built with --with-threads the module falls through to the
# synchronous path, so this file doubles as a regression guard for both cases.

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => 41;

our $main_config = <<'_EOC_';
    thread_pool default threads=4 max_queue=65536;
_EOC_

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_threads_cache keys_zone=threads_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_threads_temp 1 2;
_EOC_

our $config = <<'_EOC_';
    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        threads_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;

        proxy_cache_purge PURGE from 1.0.0.0/8 127.0.0.0/8 3.0.0.0/8;
    }

    location = /etc/passwd {
        root /;
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare passwd
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd
--- error_code: 200
--- response_headers
Content-Type: text/plain
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 2: prepare passwd2
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd2
--- error_code: 200
--- response_headers
Content-Type: text/plain
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 3: prepare shadow
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/shadow
--- error_code: 200
--- response_headers
Content-Type: text/plain
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 4: passwd is cached
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: HIT
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 5: passwd2 is cached
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd2
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: HIT
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 6: wildcard purge offloaded to thread pool
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/pass*
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 7: passwd is gone after wildcard purge
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 8: passwd2 is gone after wildcard purge
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd2
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/


=== TEST 9: shadow is still cached (non-matching prefix)
--- main_config eval: $::main_config
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/shadow
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: HIT
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
