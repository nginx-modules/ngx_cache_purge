# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * 164;

our $http_config = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags.sqlite;
_EOC_

our $http_config_hard = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_hard.sqlite;
_EOC_

our $http_config_cache_tag = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_cache_tag.sqlite;
_EOC_

our $http_config_override = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_override.sqlite;
_EOC_

our $http_config_restart = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_restart.sqlite;
_EOC_

our $http_config_plain = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_plain.sqlite;
_EOC_

our $http_config_custom = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_custom.sqlite;
_EOC_

our $http_config_multi_tag = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache keys_zone=test_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_multi_tag.sqlite;
_EOC_

our $config_multi_tag = <<'_EOC_';
    location = /proxy/multi-a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/multi-a;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /proxy/multi-b {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/multi-b;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /proxy/multi-c {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/multi-c;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /origin/multi-a {
        add_header         Surrogate-Key "sk-multi-a sk-shared-multi";
        return 200         "origin-multi-a";
    }

    location = /origin/multi-b {
        add_header         Surrogate-Key "sk-multi-b sk-shared-multi";
        return 200         "origin-multi-b";
    }

    location = /origin/multi-c {
        add_header         Surrogate-Key "sk-unrelated-multi";
        return 200         "origin-multi-c";
    }
_EOC_

our $http_config_overload = <<'_EOC_';
    proxy_cache_path  /tmp/ngx_cache_pilot_cache_overload keys_zone=overload_cache:10m;
    proxy_temp_path   /tmp/ngx_cache_pilot_temp_overload 1 2;
    map $request_method $purge_method {
        PURGE   1;
        default 0;
    }
    map $request_method $purge_never {
        default 0;
    }
    cache_pilot_index_store   sqlite /tmp/ngx_cache_pilot_tags_overload.sqlite;
_EOC_

our $config_overload = <<'_EOC_';
    location = /proxy/overload {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/overload;
        proxy_cache        overload_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /origin/overload {
        add_header         Surrogate-Key "t1";
        return 200         "origin-overload";
    }
_EOC_

our $overload_tags = join(" ", map { "t$_" } 1..1001);

our $config_soft = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /proxy/b {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/b;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /proxy/c {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/c;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
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

    location = /origin/c {
        add_header         Surrogate-Key "group-three";
        add_header         Cache-Tag "gamma";
        return 200         "origin-c";
    }
_EOC_

our $config_soft_json = $config_soft;
$config_soft_json =~ s/cache_pilot_purge_mode_header X-Purge-Mode;/cache_pilot_purge_mode_header X-Purge-Mode;\n        cache_pilot_purge_response_type json;/g;

our $config_hard = <<'_EOC_';
    location = /proxy/a {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/a;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /origin/a {
        add_header         Surrogate-Key "group-hard";
        add_header         Cache-Tag "delta, hard-only";
        return 200         "origin-a";
    }
_EOC_

our $config_plain = <<'_EOC_';
    location = /proxy/plain {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/plain;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method soft;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
    }

    location = /origin/plain {
        add_header         Surrogate-Key "group-plain";
        add_header         Cache-Tag "plain-tag";
        return 200         "origin-plain";
    }
_EOC_

our $config_custom = <<'_EOC_';
    location = /proxy/custom {
        proxy_pass         $scheme://127.0.0.1:$server_port/origin/custom;
        proxy_cache        test_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
        proxy_cache_purge  $purge_method;
        cache_pilot_purge_mode_header X-Purge-Mode;
        cache_pilot_index    on;
        cache_pilot_tag_headers  Edge-Tag Custom-Group;
    }

    location = /origin/custom {
        add_header         Edge-Tag "group-custom";
        add_header         Custom-Group "custom-alpha, custom-shared";
        return 200         "origin-custom";
    }
_EOC_

worker_connections(128);
no_shuffle();
run_tests();

no_diff();

__DATA__

=== TEST 1: prepare first soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 2: prepare second soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 3: prepare unrelated soft-tagged cache entry
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/c
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-c
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 4: headerless surrogate-key purge on soft location uses configured mode
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
PURGE /proxy/a
--- more_headers
Surrogate-Key: group-one
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 5: first matched surrogate-key entry is expired
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 6: second matched surrogate-key entry is expired
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/b
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 7: unrelated entry remains a hit after surrogate-key purge
--- http_config eval: $::http_config
--- config eval: $::config_soft
--- request
GET /proxy/c
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-c
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 8: prepare cache-tag alpha entry on fresh index
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/a?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 9: prepare cache-tag beta entry on fresh index
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/b?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 10: headerless cache-tag purge on soft location uses configured mode
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
PURGE /proxy/a?t=cache-tag
--- more_headers
Cache-Tag: alpha, missing-tag
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 11: headerless cache-tag purge expires matching alpha entry
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/a?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 12: cache-tag purge does not touch unrelated beta entry
--- http_config eval: $::http_config_cache_tag
--- config eval: $::config_soft
--- request
GET /proxy/b?t=cache-tag
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 11: prepare hard-tagged cache entry
--- http_config eval: $::http_config_hard
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 12: serve hard-tagged cache entry from cache
--- http_config eval: $::http_config_hard
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 13: hard purge by cache-tag removes the entry
--- http_config eval: $::http_config_hard
--- config eval: $::config_hard
--- request
PURGE /proxy/a?t=hard
--- more_headers
Cache-Tag: hard-only
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 14: next request after hard tag purge is a miss
--- http_config eval: $::http_config_hard
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 15: prepare hard-tagged cache entry for soft override
--- http_config eval: $::http_config_override
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard-soft
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 16: override header softens hard cache-tag purge
--- http_config eval: $::http_config_override
--- config eval: $::config_hard
--- request
PURGE /proxy/a?t=hard-soft
--- more_headers
Cache-Tag: hard-only
X-Purge-Mode: soft
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 17: soft-overridden hard cache-tag purge expires the entry
--- http_config eval: $::http_config_override
--- config eval: $::config_hard
--- request
GET /proxy/a?t=hard-soft
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
=== TEST 21: prepare restart-tagged entry for persisted bootstrap test
--- http_config eval: $::http_config_restart
--- config eval: $::config_soft
--- request
GET /proxy/a?t=restart
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 22: first tag purge succeeds after restart bootstrap
--- http_config eval: $::http_config_restart
--- config eval: $::config_soft
--- request
PURGE /proxy/a?t=restart
--- more_headers
Surrogate-Key: group-one
X-Purge-Mode: soft
--- error_code: 200
--- response_body_like: \{\"key\": 
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 23: second tag purge reuses persisted index after restart
--- http_config eval: $::http_config_restart
--- config eval: $::config_soft_json
--- request
PURGE /proxy/a?t=restart
--- more_headers
Surrogate-Key: group-one
X-Purge-Mode: soft
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: ^\{\"key\": \"\/proxy\/a\?t=restart\", \"cache_pilot\": \{\"purge_path\": \"reused-persisted-index\"\}\}$
--- grep_error_log eval
qr/cache_tag request reusing persisted index for zone "test_cache"/
--- grep_error_log_out
cache_tag request reusing persisted index for zone "test_cache"
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/




=== TEST 24: prepare watched entry for plain purge fallback
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
GET /proxy/plain
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-plain
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 25: plain PURGE without override header still works when cache_pilot_index is enabled
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
PURGE /proxy/plain
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 26: plain PURGE fallback uses configured soft mode by default
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
GET /proxy/plain
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-plain
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 27: prepare watched entry for soft override fallback
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
GET /proxy/plain?t=soft
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-plain
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 28: explicit soft override preserves plain PURGE fallback behavior
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
PURGE /proxy/plain?t=soft
--- more_headers
X-Purge-Mode: soft
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 29: plain PURGE fallback honors explicit soft override
--- http_config eval: $::http_config_plain
--- config eval: $::config_plain
--- request
GET /proxy/plain?t=soft
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-plain
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 30: prepare custom-header tagged cache entry
--- http_config eval: $::http_config_custom
--- config eval: $::config_custom
--- request
GET /proxy/custom
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-custom
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 31: serve custom-header tagged entry from cache
--- http_config eval: $::http_config_custom
--- config eval: $::config_custom
--- request
GET /proxy/custom
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-custom
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 32: custom cache_pilot_tag_headers are matched during purge
--- http_config eval: $::http_config_custom
--- config eval: $::config_custom
--- request
PURGE /proxy/custom
--- more_headers
Custom-Group: custom-alpha
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 33: custom cache_pilot_tag_headers are used for cached-response indexing
--- http_config eval: $::http_config_custom
--- config eval: $::config_custom
--- request
GET /proxy/custom
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-custom
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 34: prepare sqlite multi-tag-a cache entry
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-a
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-multi-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 35: prepare sqlite multi-tag-b cache entry
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-b
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-multi-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 36: prepare sqlite unrelated multi-tag-c cache entry
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-c
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-multi-c
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 37: sqlite purge by two tags exercises IN clause path
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
PURGE /proxy/multi-a
--- more_headers
Surrogate-Key: sk-multi-a sk-multi-b
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: \{\"key\": 
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 38: sqlite multi-tag-a entry is expired after two-tag purge
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-a
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-multi-a
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 39: sqlite multi-tag-b entry is expired after two-tag purge
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-b
--- error_code: 200
--- response_headers
X-Cache-Status: EXPIRED
--- response_body: origin-multi-b
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 40: sqlite unrelated entry remains a hit after two-tag purge
--- http_config eval: $::http_config_multi_tag
--- config eval: $::config_multi_tag
--- request
GET /proxy/multi-c
--- error_code: 200
--- response_headers
X-Cache-Status: HIT
--- response_body: origin-multi-c
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/



=== TEST 41: prepare sqlite overload cache entry for truncation test
--- http_config eval: $::http_config_overload
--- config eval: $::config_overload
--- request
GET /proxy/overload
--- error_code: 200
--- response_headers
X-Cache-Status: MISS
--- response_body: origin-overload
--- timeout: 10
--- no_error_log eval
qr/\[(error|crit|alert|emerg)\]/



=== TEST 42: sqlite purge with 1001 tags logs truncation warning at 1000
--- http_config eval: $::http_config_overload
--- config eval: $::config_overload
--- request
PURGE /proxy/overload
--- more_headers eval
"Surrogate-Key: $::overload_tags"
--- error_code: 200
--- response_body_like: \{\"key\": 
--- error_log eval
qr/cache tag: too many tags in response header, truncating at 1000/
--- no_error_log eval
qr/\[(error|crit|alert|emerg)\]/
