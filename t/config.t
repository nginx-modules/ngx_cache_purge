# t/config.t - Configuration validation tests
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

my $pwd = cwd();
$ENV{TEST_NGINX_SERVROOT} = server_root();
no_long_string();
run_tests();

__DATA__

=== TEST 1: valid background queue configuration
--- http_config
    cache_purge_background_queue on;
    cache_purge_queue_size  2048;
    cache_purge_batch_size  20;
    cache_purge_throttle_ms 25;
--- config
    location /health {
        return 200 "ok";
    }
--- request
GET /health
--- error_code: 200
--- no_error_log
[error]

=== TEST 2: invalid queue size — negative value causes config error
--- http_config
    cache_purge_background_queue on;
    cache_purge_queue_size -1;
--- config
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/invalid number/

=== TEST 3: invalid response_type value causes config error
--- http_config
--- config
    cache_purge_response_type invalid;
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/invalid parameter.*expected.*html/

=== TEST 4a: Content-Type text/html for html response type
# Separate-location syntax (proxy_cache_purge zone key) - no proxy_pass needed.
# Background queue ON + wildcard URI -> 202, exercising send_response + Content-Type.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type html;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: text/html

=== TEST 4b: Content-Type application/json for json response type
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type json;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: application/json

=== TEST 4c: Content-Type text/xml for xml response type
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type xml;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: text/xml

=== TEST 4d: Content-Type text/plain for text response type
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=ct_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type text;
    location /purge { proxy_cache_purge ct_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: text/plain
