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

=== TEST 7: wildcard partial purge — hit returns 200
# Regression test for the "always 412" bug: when matching files exist the
# wildcard purge must return 200 OK, not 412.
# Step 1: prime the cache with a known URI.
# Step 2: wildcard-purge the prefix — must return 200 because a file was deleted.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "wildcard-hit content";
    }
--- request eval
["GET /cache/wildcard7", "PURGE /cache/wildcard*"]
--- response_body eval
["wildcard-hit content", qr/purged/i]
--- error_code eval
[200, 200]

=== TEST 8: wildcard partial purge — miss with legacy_status off returns 404
# With cache_purge_legacy_status off, a wildcard miss must return 404 (not 412).
--- http_config eval: $::HttpConfig . "cache_purge_legacy_status off;"
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
PURGE /cache/no-such-entry*
--- error_code: 404

=== TEST 9: exact-key miss with legacy_status off returns 404
# Confirms ngx_http_cache_purge_not_found_code() is also respected by the
# exact-key path (ngx_http_cache_purge_handler).
--- http_config eval: $::HttpConfig . "cache_purge_legacy_status off;"
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
PURGE /cache/no-such-entry
--- error_code: 404

=== TEST 10: purge_all on populated cache returns 200
# prime two different URIs, then issue purge_all.
# The directive empties the whole zone; response must be 200 OK.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE purge_all from 127.0.0.1;
    }
    location /origin {
        return 200 "content";
    }
--- request eval
["GET /cache/pa-a", "GET /cache/pa-b", "PURGE /cache/pa-a"]
--- response_body eval
["content", "content", qr/purged/i]
--- error_code eval
[200, 200, 200]

=== TEST 11: purge_all on empty cache still returns 200
# purge_all is a zone-wide operation; even if nothing was cached the
# semantics are "the zone is now empty" — always 200, never 412/404.
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
--- response_body_like: purged
--- error_code: 200

=== TEST 12: wildcard glob-only (bare asterisk) matches all cached entries
# A key of just "*" strips the trailing asterisk, leaving an empty prefix,
# which the walk handler treats as "match everything".
# Prime one entry, then send PURGE /* — must return 200.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "glob content";
    }
--- request eval
["GET /cache/glob12", "PURGE /cache/*"]
--- response_body eval
["glob content", qr/purged/i]
--- error_code eval
[200, 200]

=== TEST 13: JSON response on wildcard hit
# Wildcard purge that deletes files must still honour cache_purge_response_type.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        cache_purge_response_type json;
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request eval
["GET /cache/json13", "PURGE /cache/json*"]
--- error_code eval
[200, 200]
--- response_headers eval
["", "Content-Type: application/json"]
--- response_body eval
["ok", qr/purged/i]

=== TEST 14: wildcard hit with legacy_status off still returns 200
# Hitting files must return 200 regardless of cache_purge_legacy_status.
--- http_config eval: $::HttpConfig . "cache_purge_legacy_status off;"
--- config
    location /cache {
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "ok";
    }
--- request eval
["GET /cache/ls14", "PURGE /cache/ls*"]
--- response_body eval
["ok", qr/purged/i]
--- error_code eval
[200, 200]
