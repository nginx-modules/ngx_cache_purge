# t/basic.t
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

our $pwd = cwd();
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

=== TEST 15: refresh keeps cached object on 304
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin304;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /origin304 {
        if ($request_method = HEAD) {
            add_header ETag '"etag-refresh-304"';
            return 304;
        }
        add_header ETag '"etag-refresh-304"';
        return 200 "refresh-304-body";
    }
--- request eval
["GET /cache/refresh304", "REFRESH /cache/refresh304", "GET /cache/refresh304"]
--- response_body eval
[
    "refresh-304-body",
    qr/Refresh: total=1 kept=1 purged=0 errors=0 .*statuses=\{304:1\}/,
    "refresh-304-body"
]
--- error_code eval
[200, 200, 200]
--- raw_response_headers_like eval
[
    undef,
    qr/X-Cache-Action: refresh\r?$/m,
    qr/X-Cache-Status: HIT\r?$/m
]

=== TEST 16: refresh purges cached object on 200 change
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin200;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /origin200 {
        if ($request_method = HEAD) {
            add_header ETag '"etag-refresh-200-v2"';
            return 200;
        }
        add_header ETag '"etag-refresh-200-v1"';
        return 200 "refresh-200-body";
    }
--- request eval
["GET /cache/refresh200", "REFRESH /cache/refresh200", "GET /cache/refresh200"]
--- response_body eval
[
    "refresh-200-body",
    qr/Refresh: total=1 kept=0 purged=1 errors=0 .*statuses=\{200:1\}/,
    "refresh-200-body"
]
--- error_code eval
[200, 200, 200]
--- raw_response_headers_like eval
[
    undef,
    qr/X-Cache-Action: refresh\r?$/m,
    qr/X-Cache-Status: MISS\r?$/m
]

=== TEST 17: refresh purges cached object on 404
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin404;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /origin404 {
        if ($request_method = HEAD) {
            return 404;
        }
        return 200 "refresh-404-body";
    }
--- request eval
["GET /cache/refresh404", "REFRESH /cache/refresh404", "GET /cache/refresh404"]
--- response_body eval
[
    "refresh-404-body",
    qr/Refresh: total=1 kept=0 purged=1 errors=0 .*statuses=\{404:1\}/,
    "refresh-404-body"
]
--- error_code eval
[200, 200, 200]
--- raw_response_headers_like eval
[
    undef,
    qr/X-Cache-Action: refresh\r?$/m,
    qr/X-Cache-Status: MISS\r?$/m
]

=== TEST 18: refresh purges cached object on 410
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        proxy_pass http://backend/origin410;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /origin410 {
        if ($request_method = HEAD) {
            return 410;
        }
        return 200 "refresh-410-body";
    }
--- request eval
["GET /cache/refresh410", "REFRESH /cache/refresh410", "GET /cache/refresh410"]
--- response_body eval
[
    "refresh-410-body",
    qr/Refresh: total=1 kept=0 purged=1 errors=0 .*statuses=\{410:1\}/,
    "refresh-410-body"
]
--- error_code eval
[200, 200, 200]
--- raw_response_headers_like eval
[
    undef,
    qr/X-Cache-Action: refresh\r?$/m,
    qr/X-Cache-Status: MISS\r?$/m
]

=== TEST 19: runtime refresh rejects purge_all-only location
--- http_config eval: $::HttpConfig
--- config
    location /cacheall {
        proxy_pass http://backend/origin;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE purge_all from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
    }
    location /origin {
        return 200 "ok";
    }
--- request
REFRESH /cacheall
--- error_code: 400
--- response_body_like: refresh_all is not enabled for this location

=== TEST 20: partial refresh purges only changed matches
--- http_config eval: $::HttpConfig
--- config
    location /cache/partial/ {
        proxy_pass http://backend/origin-partial/;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /cache/other/ {
        proxy_pass http://backend/origin-other/;
        proxy_cache cache_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /origin-partial/a.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-partial-a-v2"';
            return 200;
        }
        add_header ETag '"etag-partial-a-v1"';
        return 200 "partial-a-body";
    }
    location /origin-partial/b.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-partial-b-v1"';
            return 304;
        }
        add_header ETag '"etag-partial-b-v1"';
        return 200 "partial-b-body";
    }
    location /origin-other/c.txt {
        add_header ETag '"etag-other-c-v1"';
        return 200 "other-c-body";
    }
--- request eval
[
    "GET /cache/partial/a.txt",
    "GET /cache/partial/b.txt",
    "GET /cache/other/c.txt",
    "REFRESH /cache/partial/*",
    "GET /cache/partial/a.txt",
    "GET /cache/partial/b.txt",
    "GET /cache/other/c.txt"
]
--- response_body eval
[
    "partial-a-body",
    "partial-b-body",
    "other-c-body",
    qr/Refresh: total=2 kept=1 purged=1 errors=0 .*statuses=\{200:1,304:1\}|Refresh: total=2 kept=1 purged=1 errors=0 .*statuses=\{304:1,200:1\}/,
    "partial-a-body",
    "partial-b-body",
    "other-c-body"
]
--- error_code eval
[200, 200, 200, 200, 200, 200, 200]
--- raw_response_headers_like eval
[
    undef,
    undef,
    undef,
    qr/X-Cache-Action: refresh\r?$/m,
    qr/X-Cache-Status: MISS\r?$/m,
    qr/X-Cache-Status: HIT\r?$/m,
    qr/X-Cache-Status: HIT\r?$/m
]

=== TEST 21: refresh_all refreshes changed entries across the whole cache
--- http_config eval: $::HttpConfig . "proxy_cache_path $::pwd/cache21 levels=1:2 keys_zone=cache21_zone:10m max_size=1g inactive=60m;"
--- config
    location /cache/refreshall/ {
        proxy_pass http://backend/origin-refreshall/;
        proxy_cache cache21_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /refreshall {
        proxy_pass http://backend/;
        proxy_cache cache21_zone;
        proxy_cache_key "$uri$is_args$args";
        proxy_cache_refresh REFRESH refresh_all from 127.0.0.1;
        proxy_cache_bypass $cache_purge_refresh_bypass;
        proxy_no_cache $cache_purge_refresh_bypass;
    }
    location /origin-refreshall/keep1.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-keep1-v1"';
            return 304;
        }
        add_header ETag '"etag-keep1-v1"';
        return 200 "refreshall-keep1";
    }
    location /origin-refreshall/keep2.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-keep2-v1"';
            return 304;
        }
        add_header ETag '"etag-keep2-v1"';
        return 200 "refreshall-keep2";
    }
    location /origin-refreshall/keep3.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-keep3-v1"';
            return 304;
        }
        add_header ETag '"etag-keep3-v1"';
        return 200 "refreshall-keep3";
    }
    location /origin-refreshall/keep4.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-keep4-v1"';
            return 304;
        }
        add_header ETag '"etag-keep4-v1"';
        return 200 "refreshall-keep4";
    }
    location /origin-refreshall/change1.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-change1-v2"';
            return 200;
        }
        add_header ETag '"etag-change1-v1"';
        return 200 "refreshall-change1";
    }
    location /origin-refreshall/change2.txt {
        if ($request_method = HEAD) {
            add_header ETag '"etag-change2-v2"';
            return 200;
        }
        add_header ETag '"etag-change2-v1"';
        return 200 "refreshall-change2";
    }
--- request eval
[
    "GET /cache/refreshall/keep1.txt",
    "GET /cache/refreshall/keep2.txt",
    "GET /cache/refreshall/keep3.txt",
    "GET /cache/refreshall/keep4.txt",
    "GET /cache/refreshall/change1.txt",
    "GET /cache/refreshall/change2.txt",
    "REFRESH /refreshall",
    "GET /cache/refreshall/keep1.txt",
    "GET /cache/refreshall/change1.txt",
    "GET /cache/refreshall/change2.txt"
]
--- response_body eval
[
    "refreshall-keep1",
    "refreshall-keep2",
    "refreshall-keep3",
    "refreshall-keep4",
    "refreshall-change1",
    "refreshall-change2",
    qr/Refresh: total=6 kept=4 purged=2 errors=0 .*statuses=\{200:2,304:4\}|Refresh: total=6 kept=4 purged=2 errors=0 .*statuses=\{304:4,200:2\}/,
    "refreshall-keep1",
    "refreshall-change1",
    "refreshall-change2"
]
--- error_code eval
[200, 200, 200, 200, 200, 200, 200, 200, 200, 200]
--- raw_response_headers_like eval
[
    undef,
    undef,
    undef,
    undef,
    undef,
    undef,
    qr/X-Cache-Action: refresh\r?$/m,
    qr/X-Cache-Status: HIT\r?$/m,
    qr/X-Cache-Status: MISS\r?$/m,
    qr/X-Cache-Status: MISS\r?$/m
]
