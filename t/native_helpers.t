use strict;
use warnings;
use Test::More;
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use File::Temp qw(tempdir);
use Text::ParseWords qw(shellwords);

my $root = dirname(abs_path(__FILE__)) . '/..';
my $tmp = tempdir(CLEANUP => 1);
my @cc = (shellwords($ENV{CC} || 'cc'), '-std=gnu89', '-Wall', '-Wextra',
          '-Werror', '-Wdeclaration-after-statement', '-O1', '-g');
if ($ENV{TEST_NGINX_NO_LEAK_CHECK}) {
    die 'no-leak-check requires sanitizers' unless $ENV{TEST_NGINX_SANITIZE};
    $ENV{ASAN_OPTIONS} = ($ENV{ASAN_OPTIONS} || '') . ':detect_leaks=0';
}
my @asan = $ENV{TEST_NGINX_SANITIZE}
           ? ('-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DNGX_HAVE_NONALIGNED=0') : ();

sub read_file {
    open my $fh, '<', $_[0] or die "$_[0]: $!";
    local $/;
    return <$fh>;
}

sub write_file {
    open my $fh, '>', $_[0] or die "$_[0]: $!";
    print {$fh} $_[1] or die $!;
    close $fh or die $!;
}

sub run {
    system @_;
    is($?, 0, join(' ', @_)) or BAIL_OUT('native check failed');
}

sub capture {
    my ($log, @command) = @_;
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        open STDOUT, '>', $log or die $!;
        open STDERR, '>&', \*STDOUT or die $!;
        exec @command;
        die "exec: $!";
    }
    waitpid($pid, 0) == $pid or die "waitpid: $!";
    my $result = $?;
    return ($result, read_file($log));
}

my ($sources) = read_file("$root/config") =~ /CACHE_PURGE_SRCS="([^"]+)"/s;
die "missing module source list" unless defined $sources;
my $source = read_file("$root/ngx_cache_purge.h")
             . join "\n", map { read_file("$root/$_.c") }
               ($sources =~ /(ngx_cache_purge_\w+)\.c/g);
sub function {
    my ($name, $text) = @_;
    $text = $source unless defined $text;
    $text =~ /((?:static\s+)?(?:ngx_inline\s+)?
                 (?:ngx_uint_t|ngx_int_t|ngx_fd_t|ngx_msec_t|uint64_t|size_t|ssize_t|void|u_char|
                    ngx_http_cache_purge_(?:path|stats)_t\s+\*)\n
                 ngx_http_cache_purge_\Q$name\E\([^;]*?\n\{.*?\n})/sx
        or die "missing function: $name";
    return "$1\n\n";
}

sub platform_function {
    my ($name) = @_;
    return "#if (NGX_WIN32)\n"
           . function($name, read_file("$root/ngx_cache_purge_win32.h"))
           . "#else\n"
           . function($name, read_file("$root/ngx_cache_purge_posix.h"))
           . "#endif\n\n";
}

sub fixture { read_file("$root/t/fixtures/$_[0].c"); }

subtest 'LeakSanitizer core exception keeps unrelated leaks fatal' => sub {
    plan skip_all => 'requires process-capable LeakSanitizer'
        unless $ENV{TEST_NGINX_SANITIZE} && !$ENV{TEST_NGINX_NO_LEAK_CHECK};
    run(@cc, @asan, "$root/t/fixtures/lsan_scope.c", '-o', "$tmp/lsan-raw");
    run(@cc, @asan, "$root/t/fixtures/lsan_scope.c",
        "$root/t/fixtures/nginx_lsan.c", '-o', "$tmp/lsan-scoped");
    for my $case (['lsan-raw', 37], ['lsan-scoped', 0], ['lsan-scoped', 17, 'module']) {
        my ($exe, $leak, @args) = @$case;
        my $log = "$tmp/$exe-$leak.log";
        my ($result, $report) = capture($log, "$tmp/$exe", @args);
        if ($leak) {
            isnt($result, 0, "$exe: deliberate $leak-byte leak fails");
            like($report, qr/LeakSanitizer: detected memory leaks/,
                 'failure is a leak report');
            like($report, qr/\b$leak byte\(s\) leaked in 1 allocation/,
                 'only the expected allocation is reported');
        } else {
            is($result, 0, 'known core allocation is excepted') or diag($report);
            unlike($report, qr/ERROR: (?:AddressSanitizer|LeakSanitizer)/,
                   'no unsuppressed sanitizer error');
        }
    }
};

subtest 'configuration cycle root does not hide module or runtime leaks' => sub {
    plan skip_all => 'requires Linux and process-capable LeakSanitizer'
        unless $^O eq 'linux' && $ENV{TEST_NGINX_SANITIZE}
               && !$ENV{TEST_NGINX_NO_LEAK_CHECK};
    my $nginx = abs_path($ENV{NGINX_SOURCE} || die 'NGINX_SOURCE is required');
    my @inc = map { "-I$nginx/$_" }
              qw(src/core src/event src/event/modules src/os/unix objs);
    unshift @inc, "-I$ENV{NGINX_BUILD_DIR}" if $ENV{NGINX_BUILD_DIR};
    my @fixture = map { "$root/t/fixtures/$_.c" } qw(lsan_cycle_main lsan_cycle_stub);
    run(@cc, @asan, @inc, @fixture, '-o', "$tmp/cycle-raw");
    run(@cc, @asan, @inc, @fixture, "$root/t/fixtures/nginx_lsan_cycle.c",
        '-Wl,--wrap=ngx_init_cycle', '-o', "$tmp/cycle-rooted");
    for my $case (['cycle-raw', 'config', 'cycle'],
                  ['cycle-rooted', 'config', 0],
                  ['cycle-rooted', 'runtime', 'cycle'],
                  ['cycle-rooted', 'module', 17],
                  ['cycle-raw', 'failed', 1024],
                  ['cycle-rooted', 'failed', 0],
                  ['cycle-rooted', 'failed-module', 17],
                  ['cycle-rooted', 'failed-runtime', 1024]) {
        my ($exe, $mode, $leak) = @$case;
        my ($rc, $log) = capture("$tmp/$exe-$mode.log", "$tmp/$exe", $mode);
        if ($leak) {
            if ($leak eq 'cycle') {
                ($leak) = $log =~ /cycle bytes: (\d+)/;
                die "missing cycle size: $log" unless $leak;
            }
            isnt($rc, 0, "$exe $mode: lost allocation remains fatal") or diag($log);
            like($log, qr/LeakSanitizer: detected memory leaks/, 'leak is reported');
            like($log, qr/\b$leak byte\(s\) leaked in 1 allocation/,
                 'only the expected allocation is reported');
        } else {
            is($rc, 0, "$exe $mode: cycle result preserved") or diag($log);
            unlike($log, qr/AddressSanitizer|LeakSanitizer|runtime error:/,
                   'no sanitizer report');
        }
    }
};

subtest 'IPv4-mapped IPv6 access lists' => sub {
    write_file("$tmp/access.c", fixture('access_control_shim')
               . function('access') . fixture('access_control_main')
               . ($ENV{TEST_NGINX_NO_LEAK_CHECK} ? fixture('no_leak_check') : ''));
    run(@cc, @asan, "$tmp/access.c", '-o', "$tmp/access");
    run("$tmp/access");
};

subtest 'batch accounting across timers and thread completions' => sub {
    write_file("$tmp/batch.c", fixture('batch_scheduler_shim')
               . function('background_handler') . fixture('batch_scheduler_main'));
    run(@cc, "$tmp/batch.c", '-o', "$tmp/batch");
    run("$tmp/batch");
};

subtest 'shutdown leaves unfinished thread state intact' => sub {
    write_file("$tmp/exit.c", fixture('worker_shutdown_shim')
               . function('exit_worker') . fixture('worker_shutdown_main'));
    run(@cc, "$tmp/exit.c", '-o', "$tmp/exit");
    run("$tmp/exit");
};

subtest 'bounded traversal and allocation-error cleanup' => sub {
    $source =~ /(typedef\s+struct\s+ngx_http_cache_purge_dir_s.*?
                 }\s+ngx_http_cache_purge_cursor_t;)/sx or die 'missing cursor types';
    write_file("$tmp/walk.c", fixture('directory_walk_shim') . "$1\n\n"
               . platform_function('de_info')
               . platform_function('file_info')
               . platform_function('open_dir')
               . join("\n", map { function($_) }
                      qw(walk_name walk_open walk_close walk_step walk))
               . fixture('directory_walk_main')
               . ($ENV{TEST_NGINX_NO_LEAK_CHECK} ? fixture('no_leak_check') : ''));
    for my $relative (0, 1) {
        run(@cc, @asan, "-DNGX_CACHE_PURGE_HAVE_AT=$relative",
            "$tmp/walk.c", '-o', "$tmp/walk");
        run("$tmp/walk");
    }
};

subtest 'path bounds and 32-bit clock rollover' => sub {
    my $code = fixture('path_bounds_shim')
               . join("\n", map { platform_function($_) }
                      qw(path_separator path_char path_min_length))
               . join("\n", map { function($_) }
                      qw(path_equal path_length path_contains
                         is_protected_path is_cache_file age))
               . fixture('path_bounds_harness');
    write_file("$tmp/path.c", $code);
    for my $model ([0, 0], [1, 1], [0, 1]) {
        my ($windows, $caseless) = @$model;
        run(@cc, @asan, "-DNGX_WIN32=$windows",
            "-DNGX_HAVE_CASELESS_FILESYSTEM=$caseless",
            "$tmp/path.c", '-o', "$tmp/path");
        run("$tmp/path");
    }
};

subtest 'file-error diagnostics' => sub {
    write_file("$tmp/log.c", fixture('file_diagnostics_shim')
               . "typedef int ngx_dir_t;\n"
               . platform_function('delete_entry')
               . function('unlink') . function('open_failed')
               . fixture('file_diagnostics_main'));
    run(@cc, "$tmp/log.c", '-o', "$tmp/log");
    run("$tmp/log");
};

subtest 'pending/running deduplication, admission, metrics and recovery' => sub {
    my $nginx = $ENV{NGINX_SOURCE};
    BAIL_OUT('NGINX_SOURCE must name a configured source tree') unless $nginx;
    $nginx = abs_path($nginx);
    my @inc = map { "-I$nginx/$_" }
              qw(src/core src/event src/event/modules src/os/unix objs
                 src/http src/http/modules src/http/v2 src/http/v3);
    unshift @inc, "-I$ENV{NGINX_BUILD_DIR}" if $ENV{NGINX_BUILD_DIR};
    my ($types) = $source =~ /(typedef\s+struct\s+ngx_http_cache_purge_queue_item_s
                              .*?}\s+ngx_http_cache_purge_queue_stat_t;)/sx;
    die 'missing queue types' unless defined $types;
    my $code = "#include <ngx_config.h>\n#include <ngx_core.h>\n"
               . "#include <ngx_http.h>\n#include <assert.h>\n"
               . "#include \"$root/ngx_cache_purge_invalidation.h\"\n";
    $code .= "$1\n" while $source =~ /^(#define NGX_CACHE_PURGE_\w+[^\n]*)/mg;
    $code .= $types . "\n" . fixture('queue_state_shim');
    my $strings = read_file("$nginx/src/core/ngx_string.c");
    for my $name (qw(insert_value lookup)) {
        $strings =~ /((?:void|ngx_str_node_t\s+\*)\n
                      ngx_str_rbtree_\Q$name\E\([^;]*?\n\{.*?\n})/sx
            or die "missing core rbtree $name";
        $code .= "$1\n\n";
    }
    $code .= join("\n", map { platform_function($_) }
                  qw(path_separator path_char path_min_length));
    $code .= join("\n", map { function($_) }
                  qw(path_equal path_length path_get age stats stat_add
                     recover enqueue process_queue queue_status_snapshot probe_credit));
    $code .= fixture('queue_state_main');
    $code .= fixture('no_leak_check') if $ENV{TEST_NGINX_NO_LEAK_CHECK};
    write_file("$tmp/queue.c", $code);
    run(@cc, @asan, @inc, "$tmp/queue.c", "$nginx/src/core/ngx_rbtree.c",
        '-o', "$tmp/queue");
    run("$tmp/queue");
};

subtest 'native manager callback units and callback/data preservation' => sub {
    my $nginx = abs_path($ENV{NGINX_SOURCE} || die 'NGINX_SOURCE is required');
    my @inc = map { "-I$nginx/$_" }
              qw(src/core src/event src/event/modules src/os/unix objs);
    unshift @inc, "-I$ENV{NGINX_BUILD_DIR}" if $ENV{NGINX_BUILD_DIR};
    run(@cc, @inc, "$root/t/fixtures/manager_callback_main.c", '-o', "$tmp/manager");
    run("$tmp/manager");
};

subtest 'generation rules and conditional invalidation' => sub {
    my $nginx = abs_path($ENV{NGINX_SOURCE} || die 'NGINX_SOURCE is required');
    my @inc = map { "-I$nginx/$_" }
              qw(src/core src/event src/event/modules src/os/unix objs
                 src/http src/http/modules src/http/v2 src/http/v3);
    unshift @inc, "-I$ENV{NGINX_BUILD_DIR}" if $ENV{NGINX_BUILD_DIR};
    my $generation = read_file("$root/ngx_cache_purge_invalidation.c");
    $generation =~ /(typedef struct ngx_http_cache_purge_rule_s.*?)(?=typedef struct ngx_http_cache_purge_snapshot_s)/s
        or die 'missing generation state';
    my $code = "#include <ngx_config.h>\n#include <assert.h>\n#include <stdio.h>\n"
               . "#include \"$root/ngx_cache_purge_invalidation.h\"\n" . $1
               . fixture('generation_shim');
    my $strings = read_file("$nginx/src/core/ngx_string.c");
    $strings =~ /(ngx_int_t\nngx_hextoi\(.*?\n})/s or die 'missing hextoi';
    $code .= "$1\n";
    $code .= join("\n", map { function($_, $generation) }
                   qw(generation_lookup generation_reset generation_decode
                      generation_invalidate generation_stale generation_current));
    $code .= fixture('no_leak_check') if $ENV{TEST_NGINX_NO_LEAK_CHECK};
    write_file("$tmp/generation.c", $code . fixture('generation_main'));
    run(@cc, @asan, @inc, "$tmp/generation.c", '-o', "$tmp/generation");
    run("$tmp/generation");
};

subtest 'bounded native cache-header reader' => sub {
    my $nginx = abs_path($ENV{NGINX_SOURCE} || die 'NGINX_SOURCE is required');
    my @inc = map { "-I$nginx/$_" }
              qw(src/core src/event src/event/modules src/os/unix objs
                 src/http src/http/modules src/http/v2 src/http/v3);
    unshift @inc, "-I$ENV{NGINX_BUILD_DIR}" if $ENV{NGINX_BUILD_DIR};
    my $strings = read_file("$nginx/src/core/ngx_string.c");
    my $code = fixture('cache_entry_shim');
    for my $name (qw(hex_dump strncasecmp)) {
        $strings =~ /((?:u_char \*|ngx_int_t)\nngx_\Q$name\E\([^;]*?\n\{.*?\n})/s
            or die "missing $name";
        $code .= "$1\n";
    }
    $code .= function('read_key') . fixture('cache_entry_main');
    $code .= fixture('no_leak_check') if $ENV{TEST_NGINX_NO_LEAK_CHECK};
    write_file("$tmp/entry.c", $code);
    run(@cc, @asan, @inc, '-DNGX_HAVE_MD5=0', "$tmp/entry.c",
        "$nginx/src/core/ngx_md5.c", '-o', "$tmp/entry");
    run("$tmp/entry");
};

subtest 'native protocol and cache metadata layout' => sub {
    my $nginx = $ENV{NGINX_SOURCE};
    BAIL_OUT('NGINX_SOURCE must name a configured source tree') unless $nginx;
    $nginx = abs_path($nginx);
    my @inc = map { "-I$nginx/$_" }
              qw(src/core src/event src/event/modules src/os/unix objs
                 src/http src/http/modules src/http/v2 src/http/v3);
    unshift @inc, "-I$ENV{NGINX_BUILD_DIR}" if $ENV{NGINX_BUILD_DIR};
    my $layout = qq{#include "$root/ngx_cache_purge_module.c"\n};
    for my $protocol (qw(proxy fastcgi scgi uwsgi)) {
        my $native = read_file("$nginx/src/http/modules/ngx_http_${protocol}_module.c");
        my $header = "$nginx/src/http/modules/ngx_http_${protocol}_module.h";
        $native .= read_file($header) if -f $header;
        my (@names, @decl);
        while ($native =~ /(typedef\s+struct\s+\{\n.*?\n\}
                            \s+(ngx_http_\Q$protocol\E_[a-z_]+_t);)/sgx) {
            my ($decl, $name) = ($1, $2);
            next unless $name =~ /_(?:main_conf|loc_conf|params|headers|vars)_t$/;
            push @names, $name;
            push @decl, $decl;
        }
        die "missing $protocol layout" unless grep { /_loc_conf_t$/ } @names;
        my $text = join "\n", @decl;
        $text =~ s/\b\Q$_\E\b/upstream_$_/g for @names;
        $layout .= "$text\n";
        for my $field ('upstream', 'cache_key', "${protocol}_lengths") {
            my $type = "ngx_http_${protocol}_loc_conf_t";
            $layout .= '#if (NGX_HTTP_' . uc($protocol) . ")\n"
                       . "typedef char check_${protocol}_${field}[(offsetof($type, $field)\n"
                       . " == offsetof(upstream_$type, $field)) ? 1 : -1];\n#endif\n";
        }
    }
    write_file("$tmp/layout.c", $layout);
    run(@cc, @inc, '-c', "$tmp/layout.c", '-o', "$tmp/layout.o");
    my ($hex) = read_file("$nginx/src/core/ngx_string.c")
                =~ /(ngx_int_t\nngx_hextoi\(.*?\n})/s;
    die 'missing core hexadecimal parser' unless defined $hex;
    write_file("$tmp/metadata.c", fixture('cache_layout_metadata_check')
               . $hex . "\n" . function('invalidate_node')
               . fixture('cache_layout_metadata_main'));
    run(@cc, @inc, "$tmp/metadata.c", '-o', "$tmp/metadata");
    run("$tmp/metadata");
};

subtest 'REFRESH terminal outcomes and error isolation' => sub {
    $source =~ /(enum \{\n    NGX_REFRESH_PENDING.*?\n};)/s
        or die 'missing REFRESH result enum';
    my $code = fixture('refresh_completion_shim') . "$1\n";
    $source =~ /(static ngx_int_t\nngx_http_cache_refresh_done\([^;]*?\n\{.*?\n})/s
        or die 'missing REFRESH completion callback';
    $code .= "$1\n" . fixture('refresh_completion_main');
    $code .= fixture('no_leak_check') if $ENV{TEST_NGINX_NO_LEAK_CHECK};
    write_file("$tmp/refresh.c", $code);
    run(@cc, @asan, "$tmp/refresh.c", '-o', "$tmp/refresh");
    run("$tmp/refresh");
};

done_testing();
