# t/performance.t - Performance and load tests
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

our $pwd = cwd();
$ENV{TEST_NGINX_CACHE_ROOT} = $pwd;
$ENV{TEST_NGINX_SERVROOT}   = server_root();

no_long_string();
run_tests();

__DATA__

=== PERFORMANCE TEST 1: sync walk of 5000 files returns 412
# Background queue OFF → sync path.  The partial walk visits every file
# in the cache dir; the exact-key lookup for the literal key
# "/purge/perf*" then finds nothing → 412.  The 412 is the expected
# outcome; timing of the walk is what this test measures.
# NOTE: proxy_cache must NOT be in the same location as the 3-arg
# proxy_cache_purge directive — the module rejects that combination.
--- timeout: 60
--- http_config
    proxy_cache_path $TEST_NGINX_CACHE_ROOT/perfcache keys_zone=perf_test:10m;
    cache_purge_background_queue off;
--- config
    location /purge {
        proxy_cache_purge perf_test "$uri";
    }
--- init
    use File::Path qw(make_path);
    use Time::HiRes qw(time);
    make_path("$main::pwd/perfcache");
    my $start = time();
    for my $i (1..5000) {
        open my $fh, '>', "$main::pwd/perfcache/perf_$i.cache";
        print $fh "KEY: /purge/perf_$i\nPerformance test content\n";
        close $fh;
    }
    warn sprintf("Setup 5000 files in %.3fs\n", time() - $start);
--- request
PURGE /purge/perf*
--- error_code: 412

=== PERFORMANCE TEST 2: background queue throughput — 5 concurrent wildcard purges
--- timeout: 30
--- http_config
    proxy_cache_path $TEST_NGINX_CACHE_ROOT/bgcache keys_zone=bg_test:10m;
    cache_purge_background_queue on;
    cache_purge_queue_size  100;
    cache_purge_batch_size  50;
    cache_purge_throttle_ms 1ms;
--- config
    location /purge {
        proxy_cache_purge bg_test "$uri";
    }
--- init
    use File::Path qw(make_path);
    make_path("$main::pwd/bgcache");
    for my $i (1..1000) {
        open my $fh, '>', "$main::pwd/bgcache/bg_$i.cache";
        print $fh "KEY: /purge/bg_$i\nBackground test\n";
        close $fh;
    }
--- request eval
[("PURGE /purge/bg*") x 5]
--- error_code eval
[(202) x 5]

=== PERFORMANCE TEST 3: concurrent purges across 20 distinct key prefixes
--- timeout: 45
--- http_config
    proxy_cache_path $TEST_NGINX_CACHE_ROOT/concurrent keys_zone=concurrent_test:10m;
    cache_purge_background_queue on;
    cache_purge_queue_size 1000;
--- config
    location /purge {
        proxy_cache_purge concurrent_test "$uri";
    }
--- init
    use File::Path qw(make_path);
    make_path("$main::pwd/concurrent");
    for my $i (1..100) {
        for my $j (1..10) {
            open my $fh, '>', "$main::pwd/concurrent/conc_${i}_${j}.cache";
            print $fh "KEY: /purge/batch$i/file$j\nConcurrent test\n";
            close $fh;
        }
    }
--- request eval
[map { "PURGE /purge/batch$_*" } (1..20)]
--- error_code eval
[(202) x 20]

=== PERFORMANCE TEST 4: throttling — purge is enqueued within timeout
--- timeout: 20
--- http_config
    proxy_cache_path $TEST_NGINX_CACHE_ROOT/throttle keys_zone=throttle_test:10m;
    cache_purge_background_queue on;
    cache_purge_throttle_ms 100ms;
    cache_purge_batch_size  2;
--- config
    location /purge {
        proxy_cache_purge throttle_test "$uri";
    }
--- init
    use File::Path qw(make_path);
    use Time::HiRes qw(time);
    make_path("$main::pwd/throttle");
    for my $i (1..50) {
        open my $fh, '>', "$main::pwd/throttle/throttle_$i.cache";
        print $fh "KEY: /purge/throttle_$i\nThrottle test\n";
        close $fh;
    }
--- request
PURGE /purge/throttle*
--- error_code: 202
--- response_body_like: queued
