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

plan tests => repeat_each() * (blocks() * 4);

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
Content-Type: text/html
--- response_body_like: Successful purge
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



=== TEST 8: exact purge fans out across vary variants via key index
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Variant: a
--- request
PURGE /proxy/vary
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 9: first vary variant is a miss after exact fan-out purge
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



=== TEST 10: second vary variant is a miss after exact fan-out purge
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



=== TEST 11: exact soft purge fans out across vary variants via key index
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Purge-Mode: soft
X-Variant: a
--- request
PURGE /proxy/vary
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 12: first vary variant is expired after exact soft fan-out purge
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



=== TEST 13: second vary variant is expired after exact soft fan-out purge
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



=== TEST 14: wildcard purge removes prefix entries via key index
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/prefix-*
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 15: first prefix entry is a miss after wildcard key-index purge
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



=== TEST 16: second prefix entry is a miss after wildcard key-index purge
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



