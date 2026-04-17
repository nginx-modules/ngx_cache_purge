# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

# This file currently emits 56 assertions in total.
plan tests => repeat_each() * 56;

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_stats_cache  keys_zone=stats_test:10m;
    proxy_cache_path  /tmp/ngx_cache_pilot_stats_cache2 keys_zone=stats_test2:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_stats_temp 1 2;
_EOC_

our $config = <<'_EOC_';
    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        stats_test;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
    }

    location ~ /purge(/.*) {
        proxy_cache        stats_test;
        proxy_cache_key    $1$is_args$args;
        proxy_cache_purge  1;
    }

    location = /etc/passwd {
        root /;
    }

    location = /_stats {
        cache_pilot_stats;
    }

    location = /_stats_filtered {
        cache_pilot_stats stats_test;
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: stats endpoint returns JSON by default
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: "version"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 2: stats endpoint returns Prometheus text via ?format=prometheus
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats?format=prometheus
--- error_code: 200
--- response_headers
Content-Type: text/plain; version=0.0.4; charset=utf-8
--- response_body_like: nginx_cache_pilot_purges_total
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 3: stats endpoint returns Prometheus text via Accept: text/plain
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
Accept: text/plain
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: text/plain; version=0.0.4; charset=utf-8
--- response_body_like: nginx_cache_pilot_purges_total
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 4: stats endpoint returns JSON via ?format=json
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats?format=json
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: "zones"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: stats response sets Cache-Control: no-store
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Cache-Control: no-store
--- response_body_like: "version"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 6: non-GET returns 405
--- http_config eval: $::http_config
--- config eval: $::config
--- request
POST /_stats
--- error_code: 405
--- response_headers
Content-Type: text/html
--- response_body_like: 405
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 7: JSON response contains purges counters for all types
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: "purges".*"exact".*"wildcard".*"tag".*"all"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 9: JSON response contains zone metrics
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: "max_size".*"cold".*"entries"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 10: exact.hard counter increments after purge
--- http_config eval: $::http_config
--- config eval: $::config
--- request eval
[
    'GET /proxy/passwd?t=stats',
    'PURGE /purge/proxy/passwd?t=stats',
    'GET /_stats',
]
--- error_code eval
[200, 200, 200]
--- response_headers eval
[
    'Content-Type: text/plain',
    'Content-Type: text/html',
    'Content-Type: application/json',
]
--- response_body_like eval
[
    'root',
    'Successful purge',
    '"exact":\{"hard":[1-9]',
]
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 12: Prometheus output contains zone label for configured zone
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats?format=prometheus
--- error_code: 200
--- response_headers
Content-Type: text/plain; version=0.0.4; charset=utf-8
--- response_body_like: zone="stats_test"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 13: zone filter includes named zone
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats_filtered
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: "stats_test"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 14: zone filter excludes zones not listed
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /_stats_filtered
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_unlike: "stats_test2"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
