# t/basic.t
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

my $pwd  = cwd();
my $port = server_port();

# HttpConfig: cache zone + upstream pointing at the test server itself.
# location /cache  → cached proxy (all tests)
# location /origin → mock backend that returns test content
our $HttpConfig = qq{
    proxy_cache_path $pwd/cache levels=1:2 keys_zone=cache_zone:10m
                     max_size=1g inactive=60m;
    upstream backend {
        server 127.0.0.1:$port;
    }
};

$ENV{TEST_NGINX_SERVROOT} = server_root();
no_long_string();
run_tests();

__DATA__

=== TEST 1: cache miss returns 412
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request
PURGE /cache/test
--- error_code: 412

=== TEST 2: cache setup then purge
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "cached content";
    }
--- request eval
["GET /cache/test", "PURGE /cache/test"]
--- response_body eval
["cached content", qr/purged/]
--- error_code eval
[200, 200]

=== TEST 3: json response type on successful purge
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        cache_purge_response_type json;
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request eval
["GET /cache/test3", "PURGE /cache/test3"]
--- error_code eval
[200, 200]
--- response_headers eval
["", "Content-Type: application/json"]
--- response_body eval
["ok", qr/purged/]

=== TEST 4: access control — forbidden from unlisted IP
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_purge PURGE from 192.168.1.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request
PURGE /cache/test
--- error_code: 403

=== TEST 5: separate purge-location syntax
# /purge/<key> captures the actual cache key and passes it to
# proxy_cache_purge as the 3-arg zone+key form.  No proxy_pass is needed
# in the purge location — the zone is resolved directly from the shm table.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
    }
    location ~ ^/purge(/cache/.*) {
        allow 127.0.0.1;
        deny  all;
        proxy_cache_purge cache_zone "$1$is_args$args";
    }
    location /origin {
        return 200 "ok";
    }
--- request
PURGE /purge/cache/test
--- error_code: 412

=== TEST 6: wildcard partial purge — miss returns 412
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request
PURGE /cache/test*
--- error_code: 412
