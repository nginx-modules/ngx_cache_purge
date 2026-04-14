# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * (blocks() * 4 - 1);

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_purge_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_purge_temp 1 2;
    cache_tag_index   sqlite /tmp/ngx_cache_purge_tags.sqlite;
_EOC_

our $config = <<'_EOC_';
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

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare first tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 2: prepare second tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 3: bootstrap index and soft purge by surrogate key
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /proxy/a
--- more_headers
Surrogate-Key: group-one
--- error_code: 200
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 4: first tagged entry is expired after purge
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: second tagged entry is expired after purge
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
