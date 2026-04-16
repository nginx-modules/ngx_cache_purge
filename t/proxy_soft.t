# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * (blocks() * 4 + 22 * 1);

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
_EOC_

our $config = <<'_EOC_';
    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;

        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
    }

    location = /etc/passwd {
        root               /;
    }
_EOC_

our $config_purge_all = <<'_EOC_';
    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;

        proxy_cache_purge  $purge_method soft purge_all;
    }

    location = /etc/passwd {
        root               /;
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare cache entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 2: serve from cache
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: HIT
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 3: purge without override header uses configured soft mode
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/passwd?t=soft
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 4: next request sees expired cache entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 5: refreshed entry returns to cache hit
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: HIT
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 6: prepare soft-header cache entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft-header
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 7: explicit soft override preserves soft purge
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/passwd?t=soft-header
--- more_headers
X-Purge-Mode: soft
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 8: next request after soft-purge is expired
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft-header
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 9: hard override removes cached entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/passwd?t=soft-header
--- more_headers
X-Purge-Mode: hard
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 10: hard override removes the entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft-header
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 10A: prepare entry for repeated soft purge on stale object
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=soft-repeat
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 10B: first soft purge marks entry stale
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/passwd?t=soft-repeat
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 10C: second soft purge on stale entry does not stall cache updating
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/passwd?t=soft-repeat
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 10D: hard purge on stale entry also clears update ownership
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/passwd?t=soft-repeat
--- more_headers
X-Purge-Mode: hard
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 11: prepare first wildcard match
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=wild
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 12: prepare second wildcard match
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd2?t=wild
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 13: prepare unrelated cache entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/shadow?t=wild
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 14: headerless wildcard purge uses configured soft mode
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
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 15: first wildcard target is expired
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=wild
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 16: second wildcard target is expired
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd2?t=wild
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 17: unrelated entry remains a cache hit
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/shadow?t=wild
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: HIT
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 18: prepare first soft wildcard target
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/pass-soft-1
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 19: prepare second soft wildcard target
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/pass-soft-2
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 20: explicit soft override on wildcard entries succeeds
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/pass-soft-*
--- more_headers
X-Purge-Mode: soft
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 21: first soft wildcard target becomes expired
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/pass-soft-1
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 22: second soft wildcard target becomes expired
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/pass-soft-2
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 23: prepare first purge_all target
--- http_config eval: $::http_config
--- config eval: $::config_purge_all
--- request
GET /proxy/passwd?t=all
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 24: prepare second purge_all target
--- http_config eval: $::http_config
--- config eval: $::config_purge_all
--- request
GET /proxy/shadow?t=all
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 25: purge_all ignores override header and keeps configured soft behavior
--- http_config eval: $::http_config
--- config eval: $::config_purge_all
--- request
PURGE /proxy/any
--- more_headers
X-Purge-Mode: hard
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 26: purge_all target is served as expired
--- http_config eval: $::http_config
--- config eval: $::config_purge_all
--- request
GET /proxy/passwd?t=all
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 27: second purge_all target is served as expired
--- http_config eval: $::http_config
--- config eval: $::config_purge_all
--- request
GET /proxy/shadow?t=all
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62
