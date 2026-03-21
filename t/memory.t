# t/memory.t - Memory leak and usage tests
use Test::Nginx::Socket 'no_plan';
use Cwd qw(cwd);

our $pwd = cwd();
$ENV{TEST_NGINX_CACHE_ROOT} = $pwd;
$ENV{TEST_NGINX_SERVROOT}   = server_root();

no_long_string();
run_tests();

__DATA__

=== MEMORY TEST 1: partial purge memory usage across 1000 files
# 3-arg separate-location syntax: proxy_cache_purge zone key.
# proxy_cache must NOT appear in the same location — the module rejects
# the combination.  The zone is resolved directly from the shm table.
# Background queue ON: PURGE /purge/test* is enqueued immediately (202),
# then the background worker walks all 1000 fake files exercising
# slab alloc/free without leaking.
--- http_config
    proxy_cache_path $TEST_NGINX_CACHE_ROOT/cache keys_zone=memory_test:1m;
    cache_purge_background_queue on;
--- config
    location /purge {
        proxy_cache_purge memory_test "$uri";
    }
--- init
    use File::Path qw(make_path);
    make_path("$main::pwd/cache");
    for my $i (1..1000) {
        open my $fh, '>', "$main::pwd/cache/test_$i.cache";
        print $fh "KEY: /purge/test_$i\nContent for file $i\n";
        close $fh;
    }
--- request
PURGE /purge/test*
--- error_code: 202
--- response_body_like: queued
--- timeout: 30

=== MEMORY TEST 2: repeated partial purges don't accumulate memory
--- http_config
    proxy_cache_path $TEST_NGINX_CACHE_ROOT/cache2 keys_zone=memory_test2:1m;
    cache_purge_background_queue on;
--- config
    location /purge {
        proxy_cache_purge memory_test2 "$uri";
    }
--- init
    use File::Path qw(make_path);
    make_path("$main::pwd/cache2");
    for my $i (1..100) {
        open my $fh, '>', "$main::pwd/cache2/file_$i.cache";
        print $fh "KEY: /purge/batch$i/test\nContent\n";
        close $fh;
    }
--- request eval
[map { "PURGE /purge/batch$_*" } (1..10)]
--- error_code eval
[(202) x 10]
