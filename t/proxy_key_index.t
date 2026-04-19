# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;
use File::Path qw(remove_tree);

BEGIN {
    remove_tree('/tmp/ngx_cache_pilot_key_cache_test');
    remove_tree('/tmp/ngx_cache_pilot_key_temp_test');
    unlink '/tmp/ngx_cache_pilot_key_index_test.sqlite';
}

repeat_each(1);

plan tests => repeat_each() * 136;

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_key_cache_test keys_zone=key_cache_test:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_key_temp_test 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_key_index_test.sqlite;
_EOC_

our $config = <<'_EOC_';
    location ~ ^/proxy/(.+)$ {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/$1;
        proxy_cache        key_cache_test;
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
        cache_pilot_stats key_cache_test;
    }
_EOC_

our $config_json = $config . <<'_EOC_';
    cache_pilot_purge_response_type json;

    location ~ ^/proxy_json/(.+)$ {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/$1;
        proxy_cache        key_cache_test;
        proxy_cache_key    $uri;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index on;
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare prefix-a entry before bootstrap
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



=== TEST 2: prepare prefix-b entry before bootstrap
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



=== TEST 3: prepare vary variant a before bootstrap
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



=== TEST 4: prepare vary variant b before bootstrap
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: b
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: vary-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: soft tag purge bootstraps zone and expires vary entries
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
Surrogate-Key: group-vary
X-Purge-Mode: soft
X-Variant: a
--- request
PURGE /proxy/vary
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 6: first vary variant refreshes after bootstrap purge
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: a
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: vary-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 7: second vary variant refreshes after bootstrap purge
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: b
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: vary-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 8: stats report zone ready after bootstrap purge
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: (?s)"key_cache_test":\{.*"index":\{"state":"ready","state_code":2,"backend":"sqlite"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 9: exact soft purge still targets one direct key on indexed zone
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Purge-Mode: soft
--- request
PURGE /proxy/prefix-a
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 10: direct exact soft purge expires the targeted prefix entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/prefix-a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: prefix-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 11: direct exact soft purge leaves sibling key untouched
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/prefix-b
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: prefix-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 12: exact purge fans out across vary variants via key index
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers eval
[
    'X-Variant: a',
    '',
]
--- request eval
[
    'PURGE /proxy/vary',
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
    '(?s)"key_index":\{[^}]*"exact_fanout":[1-9]',
]
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 13: first vary variant is a miss after exact fan-out purge
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



=== TEST 14: second vary variant is a miss after exact fan-out purge
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: b
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: vary-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 15: stats report exact fanout key-index usage
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: (?s)"key_index":\{[^}]*"exact_fanout":
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 16: exact soft purge fans out across vary variants via key index
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Purge-Mode: soft
X-Variant: a
--- request
PURGE /proxy/vary
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 17: first vary variant is expired after exact soft fan-out purge
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: a
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: vary-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 18: second vary variant is expired after exact soft fan-out purge
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: b
--- request
GET /proxy/vary
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: vary-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 19: wildcard purge removes prefix entries via key index
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
    '(?s)"key_index":\{[^}]*"wildcard_hits":[1-9]',
]
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 20: first prefix entry is a miss after wildcard key-index purge
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



=== TEST 21: second prefix entry is a miss after wildcard key-index purge
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



=== TEST 22: stats report wildcard key-index usage
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



=== TEST 23: prepare JSON vary variant a for exact fan-out response coverage
--- http_config eval: $::http_config
--- config eval: $::config_json
--- more_headers
X-Variant: a
--- request
GET /proxy_json/vary
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: vary-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 24: prepare JSON vary variant b for exact fan-out response coverage
--- http_config eval: $::http_config
--- config eval: $::config_json
--- more_headers
X-Variant: b
--- request
GET /proxy_json/vary
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: vary-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 25: exact JSON purge reports exact-key-fanout
--- http_config eval: $::http_config
--- config eval: $::config_json
--- more_headers
X-Variant: a
--- request
PURGE /proxy_json/vary
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: ^\{\"key\": \"\/proxy_json\/vary\", \"cache_pilot\": \{\"purge_path\": \"exact-key-fanout\"\}\}$
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 26: first JSON vary variant is a miss after exact fan-out purge
--- http_config eval: $::http_config
--- config eval: $::config_json
--- more_headers
X-Variant: a
--- request
GET /proxy_json/vary
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: vary-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 27: second JSON vary variant is a miss after exact fan-out purge
--- http_config eval: $::http_config
--- config eval: $::config_json
--- more_headers
X-Variant: b
--- request
GET /proxy_json/vary
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: vary-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 28: prepare JSON prefix-a entry for wildcard key-index response coverage
--- http_config eval: $::http_config
--- config eval: $::config_json
--- request
GET /proxy_json/prefix-a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 29: prepare JSON prefix-b entry for wildcard key-index response coverage
--- http_config eval: $::http_config
--- config eval: $::config_json
--- request
GET /proxy_json/prefix-b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 30: wildcard JSON purge reports key-prefix-index
--- http_config eval: $::http_config
--- config eval: $::config_json
--- request
PURGE /proxy_json/prefix-*
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: ^\{\"key\": \"\/proxy_json\/prefix-\*\", \"cache_pilot\": \{\"purge_path\": \"key-prefix-index\"\}\}$
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 31: first JSON prefix entry is a miss after wildcard key-index purge
--- http_config eval: $::http_config
--- config eval: $::config_json
--- request
GET /proxy_json/prefix-a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 32: second JSON prefix entry is a miss after wildcard key-index purge
--- http_config eval: $::http_config
--- config eval: $::config_json
--- request
GET /proxy_json/prefix-b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: prefix-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



