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
    cache_purge_throttle_ms 25ms;
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
# Background queue ON + wildcard URI -> 202, exercising send_result + Content-Type.
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

=== TEST 4c: Content-Type application/xml for xml response type
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
Content-Type: application/xml

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

=== TEST 5a: throttle_ms with explicit ms suffix — accepted
--- http_config
    cache_purge_background_queue on;
    cache_purge_throttle_ms 10ms;
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200

=== TEST 5b: throttle_ms with s suffix — 1s accepted
--- http_config
    cache_purge_background_queue on;
    cache_purge_throttle_ms 1s;
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200

=== TEST 6a: response_type set in http{} context — accepted and inherited (Bug 1 + Bug 2)
# The C type includes NGX_HTTP_MAIN_CONF, so http{} is valid.
# The defunct NGX_HTTP_MODULE guard would have rejected this if it had ever fired.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=rt_zone:1m;
    cache_purge_background_queue on;
    cache_purge_response_type json;
--- config
    location /purge { proxy_cache_purge rt_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: application/json

=== TEST 6b: duplicate response_type in server{} context causes config error (Bug 3)
# Before the fix, duplicates in server{} were silently swallowed.
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=rt2_zone:1m;
--- config eval
"cache_purge_response_type html;\n    cache_purge_response_type json;"
--- must_die
--- error_log eval
qr/is duplicate/

=== TEST 6c: response_type set in server{} context inherits into location (Bug 1)
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=rt3_zone:1m;
    cache_purge_background_queue on;
--- config
    cache_purge_response_type xml;
    location /purge { proxy_cache_purge rt3_zone "$uri"; }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_headers
Content-Type: application/xml

=== TEST 20a: traffic class accepts a variable and up to three names
--- http_config
    cache_purge_traffic_class $http_user_agent bot human other;
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200
--- no_error_log
[emerg]

=== TEST 20b: traffic class accepts the braced variable form and a 32-character name
--- http_config
    cache_purge_traffic_class ${http_user_agent} xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx;
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200
--- no_error_log
[emerg]

=== TEST 20c: traffic class variable may be a map result defined after the directive
--- http_config
    cache_purge_traffic_class $tc bot human;
    map $http_user_agent $tc { default human; ~*bot bot; }
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200
--- no_error_log
[emerg]

=== TEST 21a: traffic class needs a variable, not a constant
--- http_config
    cache_purge_traffic_class bot bot human;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/needs a variable as its first parameter, got "bot"/

=== TEST 21b: traffic class refuses a lone dollar sign
--- http_config
    cache_purge_traffic_class $ bot;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/needs a variable as its first parameter/

=== TEST 21c: traffic class refuses a mixed value built from named variables
--- http_config
    map $http_user_agent $tc { default human; ~*bot bot; }
    cache_purge_traffic_class "$tc-$http_host" bot human;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/\[emerg\].*(needs a variable|unknown "tc-\$http_host" variable)/

=== TEST 21d: a mixed value starting with a prefix variable is refused
--- http_config
    cache_purge_traffic_class "$http_a-$http_b" bot;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/\[emerg\]/

=== TEST 21e: traffic class refuses an undefined variable
--- http_config
    cache_purge_traffic_class $no_such_variable bot;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/unknown "no_such_variable" variable/

=== TEST 22a: traffic class needs at least one name
--- http_config
    cache_purge_traffic_class $http_user_agent;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/invalid number of arguments in "cache_purge_traffic_class" directive/

=== TEST 22b: traffic class takes at most three names
--- http_config
    cache_purge_traffic_class $http_user_agent a b c d;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/takes at most 3 class names/

=== TEST 22c: traffic class refuses a repeated name
--- http_config
    cache_purge_traffic_class $http_user_agent bot human bot;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/duplicate traffic class name "bot"/

=== TEST 22d: traffic class refuses characters outside letters, digits, underscore and hyphen
--- http_config
    cache_purge_traffic_class $http_user_agent "b.t";
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/invalid traffic class name "b\.t"/

=== TEST 22e: traffic class refuses an empty name
--- http_config
    cache_purge_traffic_class $http_user_agent "";
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/invalid traffic class name/

=== TEST 22f: traffic class refuses a 33-character name
--- http_config
    cache_purge_traffic_class $http_user_agent xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/invalid traffic class name .* \(1\.\.32 characters\)/

=== TEST 23a: traffic class cannot be repeated
--- http_config
    cache_purge_traffic_class $http_user_agent bot;
    cache_purge_traffic_class $http_user_agent human;
--- config
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/"cache_purge_traffic_class" directive is duplicate/

=== TEST 23b: traffic class is an http-level directive only
--- http_config
    map $http_user_agent $tc { default human; }
--- config
    cache_purge_traffic_class $tc bot;
    location /health { return 200 "ok"; }
--- must_die
--- error_log eval
qr/"cache_purge_traffic_class" directive is not allowed here/

=== TEST 24a: traffic class without a status location warns that it has nothing to report
--- http_config
    cache_purge_traffic_class $http_user_agent bot;
--- config
    location /health { return 200 "ok"; }
--- request
GET /health
--- error_code: 200
--- error_log eval
qr/\[warn\].*"cache_purge_traffic_class" has nothing to report/

=== TEST 24b: traffic class with a status location does not warn
--- http_config
    proxy_cache_path $TEST_NGINX_SERVROOT/cache keys_zone=tc_zone:1m;
    cache_purge_traffic_class $http_user_agent bot;
--- config
    location /status { cache_purge_queue_status $TEST_NGINX_SERVROOT/cache; }
--- request
GET /status
--- error_code: 200
--- no_error_log
nothing to report
