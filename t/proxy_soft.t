# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * (blocks() * 4 + 16 * 1);

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp 1 2;
_EOC_

our $config = <<'_EOC_';
    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;

        proxy_cache_purge  PURGE soft from 127.0.0.1;
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

        proxy_cache_purge  PURGE soft purge_all from 127.0.0.1;
    }

    location = /etc/passwd {
        root               /;
    }
_EOC_

our $config_forbidden = <<'_EOC_';
    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;

        proxy_cache_purge  PURGE soft from 1.0.0.0/8;
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



=== TEST 3: soft purge cached entry
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



=== TEST 6: prepare first wildcard match
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
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 7: prepare second wildcard match
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
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 8: prepare unrelated cache entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/shadow
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 9: soft purge wildcard entries
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



=== TEST 10: first wildcard target is expired
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 11: second wildcard target is expired
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd2
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 12: unrelated entry remains a cache hit
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
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 13: prepare first purge_all target
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



=== TEST 14: prepare second purge_all target
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



=== TEST 15: soft purge_all expires cached entries in place
--- http_config eval: $::http_config
--- config eval: $::config_purge_all
--- request
PURGE /proxy/any
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 16: purge_all target is served as expired
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



=== TEST 17: second purge_all target is served as expired
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



=== TEST 18: prepare access-controlled soft purge entry
--- http_config eval: $::http_config
--- config eval: $::config_forbidden
--- request
GET /proxy/passwd?t=forbidden
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 19: soft purge still honors access controls
--- http_config eval: $::http_config
--- config eval: $::config_forbidden
--- request
PURGE /proxy/passwd?t=forbidden
--- error_code: 403
--- response_headers
Content-Type: text/html
--- response_body_like: 403 Forbidden
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 20: forbidden soft purge leaves entry as hit
--- http_config eval: $::http_config
--- config eval: $::config_forbidden
--- request
GET /proxy/passwd?t=forbidden
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: HIT
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62
