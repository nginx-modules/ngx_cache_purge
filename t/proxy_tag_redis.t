# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

BEGIN {
    if (system("redis-cli -h 127.0.0.1 -p 6380 ping >/dev/null 2>&1") != 0) {
        system("redis-server --save '' --appendonly no --daemonize yes --bind 127.0.0.1 --port 6380 >/dev/null 2>&1");
    }
    for my $db (0 .. 15) {
        system("redis-cli -h 127.0.0.1 -p 6380 -n $db FLUSHDB >/dev/null 2>&1");
    }
}

repeat_each(1);

plan tests => repeat_each() * 118;

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis keys_zone=redis_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=10;
_EOC_

our $http_config_hard = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis keys_zone=redis_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=11;
_EOC_

our $http_config_restart = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis keys_zone=redis_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=12;
_EOC_

our $http_config_plain = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis keys_zone=redis_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=13;
_EOC_

our $http_config_custom = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis keys_zone=redis_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=14;
_EOC_

our $http_config_cache_tag = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis keys_zone=redis_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=15;
_EOC_

our $http_config_multi_tag = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis keys_zone=redis_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=0;
_EOC_

our $config_multi_tag = <<'_EOC_';
    location = /proxy/multi-a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/multi-a;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
    }

    location = /proxy/multi-b {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/multi-b;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
    }

    location = /proxy/multi-c {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/multi-c;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
    }

    location = /origin/multi-a {
        add_header         Surrogate-Key "sk-multi-a sk-shared-multi";
        return 200         "origin-multi-a";
    }

    location = /origin/multi-b {
        add_header         Surrogate-Key "sk-multi-b sk-shared-multi";
        return 200         "origin-multi-b";
    }

    location = /origin/multi-c {
        add_header         Surrogate-Key "sk-unrelated-multi";
        return 200         "origin-multi-c";
    }
_EOC_

our $http_config_overload = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache_redis_overload keys_zone=redis_overload_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp_redis_overload 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_tag_index   redis 127.0.0.1:6380 db=1;
_EOC_

our $config_overload = <<'_EOC_';
    location = /proxy/overload {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/overload;
        proxy_cache        redis_overload_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
    }

    location = /origin/overload {
        add_header         Surrogate-Key "t1";
        return 200         "origin-overload";
    }
_EOC_

our $overload_tags = join(" ", map { "t$_" } 1..1001);

our $config_soft = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
    }

    location = /proxy/b {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/b;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
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
_EOC_

our $config_hard = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
    }

    location = /origin/a {
        add_header         Surrogate-Key "group-hard";
        add_header         Cache-Tag "delta, hard-only";
        return 200         "origin-a";
    }
_EOC_

our $config_plain = <<'_EOC_';
    location = /proxy/plain {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/plain;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
    }

    location = /origin/plain {
        add_header         Surrogate-Key "group-plain";
        add_header         Cache-Tag "plain-tag";
        return 200         "origin-plain";
    }
_EOC_

our $config_custom = <<'_EOC_';
    location = /proxy/custom {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/custom;
        proxy_cache        redis_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method;
        cache_purge_mode_header X-Purge-Mode;
        cache_tag_watch    on;
        cache_tag_headers  Edge-Tag Custom-Group;
    }

    location = /origin/custom {
        add_header         Edge-Tag "group-custom";
        add_header         Custom-Group "custom-alpha, custom-shared";
        return 200         "origin-custom";
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare first redis soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 2: prepare second redis soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-b
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 3: redis surrogate-key soft purge succeeds
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
PURGE /proxy/a
--- more_headers
Surrogate-Key: group-one
--- error_code: 200
--- response_body_like: Successful purge
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 4: first redis surrogate-key match expires
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: second redis surrogate-key match expires
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-b
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 6: prepare redis cache-tag alpha entry
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/a?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 7: prepare redis cache-tag beta entry
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/b?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-b
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 8: redis cache-tag soft purge succeeds
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
PURGE /proxy/a?t=cache-tag
--- more_headers
Cache-Tag: alpha, missing-tag
--- error_code: 200
--- response_body_like: Successful purge
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 9: redis cache-tag soft purge expires matching entry
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/a?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 10: redis cache-tag soft purge leaves unrelated entry alone
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/b?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-b
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 11: prepare redis hard-tagged cache entry
--- http_config eval: $::http_config_hard
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 12: redis hard cache-tag purge succeeds
--- http_config eval: $::http_config_hard
--- config eval: $::config_hard
--- request
PURGE /proxy/a?t=hard
--- more_headers
Cache-Tag: hard-only
--- error_code: 200
--- response_body_like: Successful purge
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 13: next request after redis hard tag purge is a miss
--- http_config eval: $::http_config_hard
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 14: prepare redis bootstrap entry
--- http_config eval: $::http_config_restart
--- config eval: $::config_soft
--- request
GET /proxy/a?t=restart
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 15: first redis tag purge bootstraps the zone index
--- http_config eval: $::http_config_restart
--- config eval: $::config_soft
--- request
PURGE /proxy/a?t=restart
--- more_headers
Surrogate-Key: group-one
X-Purge-Mode: soft
--- error_code: 200
--- response_body_like: Successful purge
--- grep_error_log eval
qr/cache_tag bootstrap zone "redis_cache"/
--- grep_error_log_out
cache_tag bootstrap zone "redis_cache"
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 16: second redis tag purge reuses persisted index
--- http_config eval: $::http_config_restart
--- config eval: $::config_soft
--- request
PURGE /proxy/a?t=restart
--- more_headers
Surrogate-Key: group-one
X-Purge-Mode: soft
--- error_code: 200
--- response_body_like: Successful purge
--- grep_error_log eval
qr/cache_tag request reusing persisted index for zone "redis_cache"/
--- grep_error_log_out
cache_tag request reusing persisted index for zone "redis_cache"
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 17: prepare redis plain purge fallback entry
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
GET /proxy/plain
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-plain
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 18: plain PURGE still works with redis cache_tag_watch
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
PURGE /proxy/plain
--- error_code: 200
--- response_body_like: Successful purge
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 19: plain PURGE fallback still uses soft mode with redis backend
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
GET /proxy/plain
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-plain
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 20: prepare redis custom-header tagged entry
--- http_config eval: $::http_config_custom
--- config eval: $::config_custom
--- request
GET /proxy/custom
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-custom
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 21: redis custom cache_tag_headers are matched during purge
--- http_config eval: $::http_config_custom
--- config eval: $::config_custom
--- request
PURGE /proxy/custom
--- more_headers
Custom-Group: custom-alpha
--- error_code: 200
--- response_body_like: Successful purge
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 22: redis custom cache_tag_headers are used for cached-response indexing
--- http_config eval: $::http_config_custom
--- config eval: $::config_custom
--- request
GET /proxy/custom
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-custom
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 23: prepare redis multi-tag-a cache entry
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-multi-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 24: prepare redis multi-tag-b cache entry
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-multi-b
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 25: prepare redis unrelated multi-tag-c cache entry
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-c
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-multi-c
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 26: redis purge by two tags uses SUNION across two tag keys
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
PURGE /proxy/multi-a
--- more_headers
Surrogate-Key: sk-multi-a sk-multi-b
--- error_code: 200
--- response_body_like: Successful purge
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 27: redis multi-tag-a entry is expired after two-tag purge
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-multi-a
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 28: redis multi-tag-b entry is expired after two-tag purge
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-b
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-multi-b
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 29: redis unrelated entry remains a hit after two-tag purge
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-c
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-multi-c
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 30: prepare redis overload cache entry for truncation test
--- http_config eval: $::http_config_overload
--- config eval: $::config_overload
--- request
GET /proxy/overload
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-overload
--- timeout: 10
--- no_error_log eval
qr/\[(error|crit|alert|emerg)\]/



=== TEST 31: redis purge with 1001 tags logs truncation warning at 1000
--- http_config eval: $::http_config_overload
--- config eval: $::config_overload
--- request
PURGE /proxy/overload
--- more_headers eval
"Surrogate-Key: $::overload_tags"
--- error_code: 200
--- response_body_like: Successful purge
--- error_log eval
qr/cache tag: too many tags in response header, truncating at 1000/
--- no_error_log eval
qr/\[(error|crit|alert|emerg)\]/
