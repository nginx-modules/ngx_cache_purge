# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;
use File::Path qw(remove_tree);

BEGIN {
    remove_tree('/tmp/ngx_cache_pilot_key_cache_redis_test');
    remove_tree('/tmp/ngx_cache_pilot_key_temp_redis_test');
    if (system("redis-cli -h 127.0.0.1 -p 6380 ping >/dev/null 2>&1") != 0) {
        system("redis-server --save '' --appendonly no --daemonize yes --bind 127.0.0.1 --port 6380 >/dev/null 2>&1");
    }
    system("redis-cli -h 127.0.0.1 -p 6380 -n 2 FLUSHDB >/dev/null 2>&1");
}

repeat_each(1);

plan tests => repeat_each() * 40;

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_key_cache_redis_test keys_zone=key_cache_redis_test:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_key_temp_redis_test 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    cache_pilot_index_store   redis 127.0.0.1:6380 db=2;
_EOC_

our $config = <<'_EOC_';
    location ~ ^/proxy/(.+)$ {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/$1;
        proxy_cache        key_cache_redis_test;
        proxy_cache_key    $uri;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index on;
    }

    location = /origin/vary {
        add_header         Surrogate-Key "group-vary";
        add_header         Vary X-Variant always;
        return 200         "vary-$http_x_variant";
    }

    location = /origin/prefix-a {
        add_header         Surrogate-Key "group-prefix";
        return 200         "prefix-a";
    }

    location = /origin/prefix-b {
        add_header         Surrogate-Key "group-prefix";
        return 200         "prefix-b";
    }

    location = /_stats {
        cache_pilot_stats key_cache_redis_test;
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare redis prefix-a entry before bootstrap
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/prefix-a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 2: prepare redis prefix-b entry before bootstrap
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/prefix-b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 3: prepare unrelated redis vary variant a
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: a
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: vary-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 4: redis wildcard purge removes prefix entries via key index
--- http_config eval: $::http_config
--- config eval: $::config
--- request eval
[
    'PURGE /proxy/prefix-*',
    'GET /_stats',
]
--- error_code eval
[200, 200]
--- response_headers eval
[
    'Content-Type: application/json',
    'Content-Type: application/json',
]
--- response_body_like eval
[
    '{"key": ',
    '(?s)"key_index":\{[^}]*"wildcard_hits":[1-9].*"key_cache_redis_test":\{.*"index":\{"state":"ready","state_code":2,"backend":"redis"',
]
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: redis first prefix entry is a miss after wildcard key-index purge
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/prefix-a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 6: redis second prefix entry is a miss after wildcard key-index purge
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/prefix-b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 7: redis unrelated vary variant remains a hit after wildcard purge
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: a
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: vary-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 8: redis stats report wildcard key-index usage
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: (?s)"key_index":\{[^}]*"wildcard_hits":
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 9: redis stats report ready zone
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: (?s)"key_cache_redis_test":\{.*"index":\{"state":"ready","state_code":2,"backend":"redis"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



