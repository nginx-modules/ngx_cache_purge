# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(1);

plan tests => repeat_each() * (blocks() * 4 + 5 * 1);

our $http_config = <<'_EOC_';
    proxy_cache_path    /tmp/ngx_cache_purge_proxy_cache keys_zone=proxy_cache:10m;
    proxy_temp_path     /tmp/ngx_cache_purge_proxy_temp 1 2;
    fastcgi_cache_path  /tmp/ngx_cache_purge_fastcgi_cache keys_zone=fastcgi_cache:10m;
    scgi_cache_path     /tmp/ngx_cache_purge_scgi_cache keys_zone=scgi_cache:10m;
    uwsgi_cache_path    /tmp/ngx_cache_purge_uwsgi_cache keys_zone=uwsgi_cache:10m;
_EOC_

our $config = <<'_EOC_';
    cache_purge_response_type json;

    location /proxy {
        proxy_pass         $scheme://127.0.0.1:$server_port/etc/passwd;
        proxy_cache        proxy_cache;
        proxy_cache_key    $uri$is_args$args;
        proxy_cache_valid  3m;
        add_header         X-Cache-Status $upstream_cache_status;
    }

    location ~ /purge_proxy(/.*) {
        proxy_cache_purge          proxy_cache $1$is_args$args soft;
        cache_purge_response_type  html;
    }

    location ~ /purge_proxy_json(/.*) {
        proxy_cache_purge          proxy_cache $1$is_args$args soft;
    }

    location ~ /purge_proxy_xml(/.*) {
        proxy_cache_purge          proxy_cache $1$is_args$args soft;
        cache_purge_response_type  xml;
    }

    location ~ /purge_proxy_text(/.*) {
        proxy_cache_purge          proxy_cache $1$is_args$args soft;
        cache_purge_response_type  text;
    }

    location ~ /purge_fastcgi(/.*) {
        fastcgi_cache_purge        fastcgi_cache $1$is_args$args soft;
    }

    location ~ /purge_scgi(/.*) {
        scgi_cache_purge           scgi_cache $1$is_args$args soft;
    }

    location ~ /purge_uwsgi(/.*) {
        uwsgi_cache_purge          uwsgi_cache $1$is_args$args soft;
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

=== TEST 1: prepare proxy soft purge target
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=proxy-soft-html
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 2: separate-location proxy soft purge succeeds
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /purge_proxy/proxy/passwd?t=proxy-soft-html
--- error_code: 200
--- response_headers
Content-Type: text/html
--- response_body_like: Successful purge
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 3: separate-location proxy soft purge expires entry
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=proxy-soft-html
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: EXPIRED
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 4: prepare JSON soft purge target
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=proxy-soft-json
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 5: soft purge keeps JSON response type
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /purge_proxy_json/proxy/passwd?t=proxy-soft-json
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body_like: {\"Key\": \"\/proxy\/passwd\?t=proxy-soft-json\"
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 6: prepare XML soft purge target
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=proxy-soft-xml
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 7: soft purge keeps XML response type
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /purge_proxy_xml/proxy/passwd?t=proxy-soft-xml
--- error_code: 200
--- response_headers
Content-Type: text/xml
--- response_body_like: \<\?xml version=\"1.0\" encoding=\"UTF-8\"\?><status><Key><\!\[CDATA\[\/proxy\/passwd\?t=proxy-soft-xml\]\]><\/Key>
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 8: prepare text soft purge target
--- http_config eval: $::http_config
--- config eval: $::config
--- request
GET /proxy/passwd?t=proxy-soft-text
--- error_code: 200
--- response_headers
Content-Type: text/plain
X-Cache-Status: MISS
--- response_body_like: root
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 5: < 0.8.3 or < 0.7.62



=== TEST 9: soft purge keeps text response type
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /purge_proxy_text/proxy/passwd?t=proxy-soft-text
--- error_code: 200
--- response_headers
Content-Type: text/plain
--- response_body_like: Key
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 10: fastcgi separate-location syntax accepts soft
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /purge_fastcgi/missing?t=fastcgi-soft
--- error_code: 412
--- response_headers
Content-Type: text/html
--- response_body_like: 412 Precondition Failed
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 11: scgi separate-location syntax accepts soft
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /purge_scgi/missing?t=scgi-soft
--- error_code: 412
--- response_headers
Content-Type: text/html
--- response_body_like: 412 Precondition Failed
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62



=== TEST 12: uwsgi separate-location syntax accepts soft
--- http_config eval: $::http_config
--- config eval: $::config
--- request
PURGE /purge_uwsgi/missing?t=uwsgi-soft
--- error_code: 412
--- response_headers
Content-Type: text/html
--- response_body_like: 412 Precondition Failed
--- timeout: 10
--- no_error_log eval
qr/\[(warn|error|crit|alert|emerg)\]/
--- skip_nginx2: 4: < 0.8.3 or < 0.7.62