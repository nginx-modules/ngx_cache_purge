use strict;
use warnings;
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use File::Path qw(make_path);
use File::Temp qw(tempdir);
use Getopt::Long qw(GetOptions);
use Text::ParseWords qw(shellwords);

my ($source, $sanitize, $no_leak_check, $allow_initgroups, $threads);
my $cc = $ENV{CC} || 'cc';
my $workers = 8;
my $files = 4096;
GetOptions('nginx-source=s' => \$source, 'cc=s' => \$cc,
           'workers=i' => \$workers, 'files=i' => \$files,
           'sanitize' => \$sanitize,
           'threads' => \$threads,
           'no-leak-check' => \$no_leak_check,
           'allow-initgroups-warning' => \$allow_initgroups)
    && defined $source or die "usage: perl t/build_verify.pl --nginx-source PATH\n";
$source = abs_path($source) or die 'invalid source path';
my $module = abs_path(dirname(__FILE__) . '/..');
my @asan = $sanitize ? ('-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-DNGX_HAVE_NONALIGNED=0') : ();
my @core = ('-O1', '-g', '-pipe', '-Wall', '-Wno-unused-parameter', @asan);
my @strict = ('-std=gnu89', '-O1', '-g', '-pipe', '-Wall', '-Wextra',
              '-Werror', '-Wdeclaration-after-statement', @asan);

sub run {
    print '+ ', join(' ', @_), "\n";
    system @_;
    die "command failed: $?\n" if $?;
}

sub read_file {
    open my $fh, '<', $_[0] or die "$_[0]: $!";
    local $/;
    return <$fh>;
}

my @ld = @asan;
die '--no-leak-check requires --sanitize' if $no_leak_check && !$sanitize;
if ($sanitize) {
    my $fixture = $no_leak_check ? 'no_leak_check' : 'nginx_lsan';
    my $tmp = tempdir(CLEANUP => 1);
    run(shellwords($cc), '-c', "$module/t/fixtures/$fixture.c",
        '-o', "$tmp/$fixture.o");
    push @ld, "$tmp/$fixture.o";
}

chdir $source or die $!;
run('./configure', "--with-cc=$cc", '--with-cc-opt=' . join(' ', @core),
    '--with-ld-opt=' . join(' ', @ld), '--without-http_rewrite_module',
    '--without-http_auth_basic_module',
    '--http-client-body-temp-path=client_body_temp', '--with-debug',
    ($threads ? ('--with-threads') : ()),
    "--add-module=$module");
my $config = read_file('objs/ngx_auto_config.h');
for (qw(CACHE PROXY FASTCGI SCGI UWSGI)) {
    die "missing feature: $_" unless $config =~ /#define NGX_HTTP_$_\s+1\b/;
}
my $makefile = read_file('objs/Makefile');
my @includes = map { "-I$_" }
               qw(src/core src/event src/event/modules src/os/unix objs
                  src/http src/http/modules);
my @link;
if ($sanitize && $^O eq 'linux') {
    my $object = 'objs/nginx_lsan_cycle.o';
    run(shellwords($cc), @strict, @includes, '-c',
        "$module/t/fixtures/nginx_lsan_cycle.c", '-o', $object);
    @link = ("LINK=$cc $object -Wl,--wrap=ngx_init_cycle");
}
my ($sources) = read_file("$module/config") =~ /CACHE_PURGE_SRCS="([^"]+)"/s;
die "missing module source list" unless defined $sources;
for my $unit ($sources =~ /(ngx_cache_purge_\w+)\.c/g) {
    $makefile =~ /(\S*addon\/\S*\Q$unit\E\.o)/ or die "missing object: $unit";
    my $target = $1;
    make_path(dirname($target));
    run(shellwords($cc), @strict, @includes, '-c', "$module/$unit.c", '-o', $target);
    for my $guards (['-DNGX_DEBUG=0'], ['-DNGX_HTTP_CACHE=0'],
                    ['-DNGX_HTTP_PROXY=0'],
                    [map { "-DNGX_HTTP_$_=0" } qw(PROXY FASTCGI SCGI UWSGI)]) {
        run(shellwords($cc), @strict, @$guards, @includes,
            '-c', "$module/$unit.c", '-o', "$target.guards-check");
        unlink "$target.guards-check" or die $!;
    }
}
run('make', '-f', 'objs/Makefile', '-j2', 'objs/nginx',
    'CFLAGS=' . join(' ', @core), @link);
$ENV{CC} = $cc;
$ENV{NGINX_SOURCE} = $source;
$ENV{TEST_NGINX_BINARY} = "$source/objs/nginx";
$ENV{TEST_NGINX_WORKERS} = $workers;
$ENV{TEST_NGINX_FILES} = $files;
$ENV{TEST_NGINX_SANITIZE} = $sanitize || 0;
$ENV{TEST_NGINX_NO_LEAK_CHECK} = $no_leak_check || 0;
$ENV{TEST_NGINX_THREADS} = $threads || 0;
$ENV{TEST_NGINX_ALLOW_INITGROUPS_WARNING} = $allow_initgroups || 0;
chdir $module or die $!;
run('sh', "$module/t/run_tests.sh", 't/native_helpers.t', 't/purge_runtime.t');
