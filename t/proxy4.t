# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => 41;

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   on;
        default off;
    }
    map $request_method $purge_never {
        default off;
    }
_EOC_

our $config = <<'_EOC_';
    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;

        proxy_cache_purge  $purge_method;
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

=== TEST 1: prepare passwd
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


=== TEST 4: get from cache passwd
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


=== TEST 5: get from cache passwd2
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


=== TEST 6: purge from cache
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


=== TEST 7: get from empty cache passwd
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


=== TEST 8: get from empty cache passwd2
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


=== TEST 9: get from cache shadow
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
