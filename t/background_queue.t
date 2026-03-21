# t/background_queue.t
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

my $pwd  = cwd();
my $port = server_port();

# HttpConfig: cache zone + queue config + upstream.
# Tests that need a live origin use location /origin.
# Tests that only send PURGE use location /cache without proxy_pass
# (proxy_cache alone is sufficient for the inline purge handler to
#  resolve the cache zone).
our $HttpConfig = qq{
    proxy_cache_path $pwd/cache levels=1:2 keys_zone=cache_zone:10m
                     max_size=1g inactive=60m;
    cache_purge_background_queue on;
    cache_purge_queue_size       100;
    cache_purge_batch_size       5;
    cache_purge_throttle_ms      10;
    upstream backend {
        server 127.0.0.1:$port;
    }
};

$ENV{TEST_NGINX_SERVROOT} = server_root();
no_long_string();
run_tests();

__DATA__

=== TEST 1: wildcard purge is enqueued — returns 202 with JSON status
--- http_config eval: $::HttpConfig
--- config
    cache_purge_response_type json;
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "test content";
    }
--- request eval
["GET /cache/test", "PURGE /cache/test*"]
--- response_body eval
["test content", qr/"Status":\s*"queued"/]
--- error_code eval
[200, 202]

=== TEST 2: invalid queue_size value causes config error
--- http_config
    cache_purge_background_queue on;
    cache_purge_queue_size invalid;
--- config
    location /health {
        return 200 "ok";
    }
--- must_die
--- error_log eval
qr/invalid number/

=== TEST 3: queue overflow — second wildcard purge falls back to 412
# queue_size=1 so the second enqueue attempt is rejected; the sync
# fallback finds no cached entry for that key and returns 412.
--- http_config
    cache_purge_background_queue on;
    cache_purge_queue_size  1;
    cache_purge_batch_size  1;
    cache_purge_throttle_ms 1000;
    proxy_cache_path $TEST_NGINX_SERVROOT/cache levels=1:2
                     keys_zone=qs_zone:1m;
--- config
    location /cache {
        proxy_cache     qs_zone;
        proxy_cache_key "$uri";
        proxy_cache_purge PURGE from 127.0.0.1;
    }
--- request eval
["PURGE /cache/test1*", "PURGE /cache/test2*"]
--- error_code eval
[202, 412]

=== TEST 4: purge_all is enqueued
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_purge PURGE purge_all from 127.0.0.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request
PURGE /cache/anything
--- error_code: 202
--- response_body_like: queued

=== TEST 5: background queue off — sync purge returns 412 on miss
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache levels=1:2
                     keys_zone=sync_zone:10m;
    cache_purge_background_queue off;
--- config
    location /cache {
        proxy_cache     sync_zone;
        proxy_cache_key "$uri";
        proxy_cache_purge PURGE from 127.0.0.1;
    }
--- request
PURGE /cache/test
--- error_code: 412

=== TEST 6: duplicate wildcard purge requests both accepted
--- http_config eval: $::HttpConfig
--- config
    cache_purge_response_type text;
    location /cache {
        proxy_cache     cache_zone;
        proxy_cache_key "$uri";
        proxy_cache_purge PURGE from 127.0.0.1;
    }
--- request eval
["PURGE /cache/same*", "PURGE /cache/same*"]
--- error_code eval
[202, 202]
--- response_body eval
[qr{Key: /cache/same}, qr{Key: /cache/same}]
