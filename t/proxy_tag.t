# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * (blocks() * 4);

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp 1 2;
    cache_tag_index   sqlite /tmp/ngx_cache_purge_tags.sqlite;
_EOC_

our $config_soft = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  PURGE soft from 127.0.0.1;
        cache_tag_watch    on;
    }

    location = /proxy/b {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/b;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  PURGE soft from 127.0.0.1;
        cache_tag_watch    on;
    }

    location = /proxy/c {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/c;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  PURGE soft from 127.0.0.1;
        cache_tag_watch    on;
    }

    location = /origin/a {
        add_header         Surrogate-Key "group-one common";
        add_header         Cache-Tag "alpha, shared";
        return 200         "origin-a";
    }

    location = /origin/b {
        add_header         Surrogate-Key "group-one";
        add_header         Cache-Tag "beta, shared";
        return 200         "origin-b";
    }

    location = /origin/c {
        add_header         Surrogate-Key "group-three";
        add_header         Cache-Tag "gamma";
        return 200         "origin-c";
    }
_EOC_

our $config_hard = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  PURGE from 127.0.0.1;
        cache_tag_watch    on;
    }

    location = /origin/a {
        add_header         Surrogate-Key "group-hard";
        add_header         Cache-Tag "delta, hard-only";
        return 200         "origin-a";
    }
_EOC_

our $config_forbidden = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  PURGE soft from 1.0.0.0/8;
        cache_tag_watch    on;
    }

    location = /origin/a {
        add_header         Surrogate-Key "group-denied";
        add_header         Cache-Tag "denied";
        return 200         "origin-a";
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare first soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 2: prepare second soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 3: prepare unrelated soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/c
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-c
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 4: soft purge by surrogate key with bootstrap lookup
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
PURGE /proxy/a
--- more_headers
Surrogate-Key: group-one
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: first matched surrogate-key entry is expired
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 6: second matched surrogate-key entry is expired
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 7: unrelated entry remains a hit after surrogate-key purge
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/c
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-c
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 8: soft purge by cache-tag comma parsing
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
PURGE /proxy/a
--- more_headers
Cache-Tag: alpha, missing-tag
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 9: cache-tag purge expires matching alpha entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 10: cache-tag purge does not touch unrelated beta entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 11: prepare hard-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 12: serve hard-tagged cache entry from cache
--- http_config eval: $::http_config
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 13: hard purge by cache-tag removes the entry
--- http_config eval: $::http_config
--- config eval: $::config_hard
--- request
PURGE /proxy/a?t=hard
--- more_headers
Cache-Tag: hard-only
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 14: next request after hard tag purge is a miss
--- http_config eval: $::http_config
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 15: prepare forbidden tag purge entry
--- http_config eval: $::http_config
--- config eval: $::config_forbidden
--- request
GET /proxy/a?t=forbidden
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 16: forbidden tag purge is rejected
--- http_config eval: $::http_config
--- config eval: $::config_forbidden
--- request
PURGE /proxy/a?t=forbidden
--- more_headers
Surrogate-Key: group-denied
--- error_code: 403
--- response_headers
Content-Type: text/html
--- response_body_like: 403 Forbidden
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 17: forbidden tag purge leaves cached entry as hit
--- http_config eval: $::http_config
--- config eval: $::config_forbidden
--- request
GET /proxy/a?t=forbidden
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
