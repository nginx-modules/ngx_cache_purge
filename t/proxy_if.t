# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => 20;

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_if_cache keys_zone=if_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_if_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
_EOC_

our $config = <<'_EOC_';
    location /proxy {
        set $flag "0";
        if ($http_x_trigger_if) {
            set $flag "1";
        }

        proxy_pass         $scheme://127.0.0.1:$server_port/origin;
        proxy_cache        if_cache;
        proxy_cache_key    $uri;
        proxy_cache_valid  3m;
        proxy_cache_purge  $purge_method;
        add_header         X-Cache-Status $upstream_cache_status always;
    }

    location /origin {
        return 200 "if-proxy";
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: GET proxies correctly without if branch
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/if
--- error_code: 200
--- response_body: if-proxy
--- response_headers
X-Cache-Status: MISS
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 2: GET proxies correctly through if child location
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Trigger-If: 1
--- request
GET /proxy/if-if
--- error_code: 200
--- response_body: if-proxy
--- response_headers
X-Cache-Status: MISS
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 3: cached response is served through if child location
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Trigger-If: 1
--- request
GET /proxy/if-if
--- error_code: 200
--- response_body: if-proxy
--- response_headers
X-Cache-Status: HIT
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 4: PURGE still works through if child location
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Trigger-If: 1
--- request
PURGE /proxy/if-if
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\":
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: purged cached response is fetched again through if child location
--- http_config eval: $::http_config
--- config eval: $::config
--- more_headers
X-Trigger-If: 1
--- request
GET /proxy/if-if
--- error_code: 200
--- response_body: if-proxy
--- response_headers
X-Cache-Status: MISS
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
