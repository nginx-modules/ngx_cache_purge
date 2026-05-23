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

=== TEST 15: vary_aware purge succeeds after real cache warm
# cache_purge_vary_aware is a main-context directive (NGX_HTTP_MAIN_CONF).
# It makes the module walk the cache shard directory after deleting the
# primary entry, removing any variant files sharing the same KEY: line.
#
# Sequence: GET warms the cache (rbtree node created), first PURGE finds
# the node, deletes the primary file, and walks for variants (200), second
# PURGE confirms the entry is gone (412).
--- http_config eval: $::HttpConfig . "    cache_purge_vary_aware on;\n"
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
["GET /cache/vary-doc", "PURGE /cache/vary-doc", "PURGE /cache/vary-doc"]
--- error_code eval
[200, 200, 412]
--- no_error_log
[error]

=== TEST 16: if-block empty body -- GET proxies correctly (was 404 in v3+)
# Regression test for the merge_loc_conf if-child handler bug.
# When nginx processes "if (cond) {}" inside a location, it creates an
# anonymous child location.  Before this fix, merge_loc_conf saved NULL
# as original_handler for the child (clcf->handler is NULL in an if-block
# with no handler directive).  Non-PURGE requests entering the if-branch
# then returned 404 instead of reaching proxy_pass.
#
# With the fix, was_set_proxy is false for the child (proxy_cache_purge
# was not configured inside the if block), so original_handler is
# inherited from prev -- which holds the real ngx_http_proxy_handler.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        if ($arg_x_trigger_if) {
        }
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "if-empty";
    }
--- request eval
["GET /cache/if16a", "GET /cache/if16b?x_trigger_if=1"]
--- error_code eval
[200, 200]
--- response_body eval
["if-empty", "if-empty"]
--- no_error_log
[error]

=== TEST 17: if-block with set directive -- GET proxies correctly
# "set $var value" inside an if block is the documented safe use of if.
# This is the exact pattern from the bug report: the if block only sets
# a variable and does nothing else, yet triggered 404 in v3+.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        set $flag "0";
        if ($arg_x_trigger_if) {
            set $flag "1";
        }
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "if-set";
    }
--- request eval
["GET /cache/if17a", "GET /cache/if17b?x_trigger_if=1"]
--- error_code eval
[200, 200]
--- response_body eval
["if-set", "if-set"]
--- no_error_log
[error]

=== TEST 18: PURGE still works when if condition is true
# The fix must not break purge requests that arrive while an if condition
# is active.  When method == PURGE and the if-branch is taken, the
# access_handler must still dispatch to the purge handler, not forward
# to the original upstream handler.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        set $flag "0";
        if ($arg_x_trigger_if) {
            set $flag "1";
        }
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "if-purge";
    }
--- request eval
["GET /cache/if18", "PURGE /cache/if18?x_trigger_if=1", "PURGE /cache/if18"]
--- error_code eval
[200, 200, 412]
--- response_body eval
["if-purge", qr/purged/i, qr/404|412/]
--- no_error_log
[error]

=== TEST 19: two sequential if blocks in the same location
# Each "if" block creates a separate anonymous child location.  Both
# must correctly inherit original_handler from the parent so that
# requests entering either branch are proxied, not 404'd.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        set $a "0";
        set $b "0";
        if ($arg_x_a) {
            set $a "1";
        }
        if ($arg_x_b) {
            set $b "1";
        }
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri$is_args$args";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
    }
    location /origin {
        return 200 "two-ifs";
    }
--- request eval
[
    "GET /cache/if19a",
    "GET /cache/if19b?x_a=1",
    "GET /cache/if19c?x_b=1",
    "GET /cache/if19d?x_a=1&x_b=1"
]
--- error_code eval
[200, 200, 200, 200]
--- response_body eval
["two-ifs", "two-ifs", "two-ifs", "two-ifs"]
--- no_error_log
[error]

=== TEST 20: cached response served from cache when if condition is true
# Confirms that a cached response is returned (HIT) and not forwarded to
# the upstream when the if condition fires.  Verifies that the cache
# lookup itself is not bypassed by the if-child handler switch.
--- http_config eval: $::HttpConfig
--- config
    location /cache {
        if ($arg_x_trigger_if) {
            set $unused "1";
        }
        proxy_pass        http://backend/origin;
        proxy_cache       cache_zone;
        proxy_cache_key   "$uri";
        proxy_cache_valid 200 1m;
        proxy_cache_purge PURGE from 127.0.0.1;
        add_header X-Cache-Status $upstream_cache_status always;
    }
    location /origin {
        return 200 "cached-body";
    }
--- request eval
["GET /cache/if20", "GET /cache/if20?x_trigger_if=1"]
--- error_code eval
[200, 200]
--- response_body eval
["cached-body", "cached-body"]
--- response_headers_like eval
["X-Cache-Status: MISS", "X-Cache-Status: HIT"]
--- no_error_log
[error]
