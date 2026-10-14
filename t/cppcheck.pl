use strict;
use warnings;
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use JSON::PP qw(encode_json);

my $root = abs_path(dirname(__FILE__) . '/..');
my $nginx = $ENV{NGINX_SOURCE} || '/opt/nginx';
chdir $nginx or die $!;
system('./configure', '--with-threads', '--with-debug',
       '--without-http_rewrite_module', '--without-http_auth_basic_module',
       "--add-module=$root") == 0 or die 'configure failed';
open my $config, '<', "$root/config" or die $!;
my $text = do { local $/; <$config> };
my ($sources) = $text =~ /CACHE_PURGE_SRCS="([^"]+)"/s;
die 'missing source manifest' unless defined $sources;
my @includes = map { "-I$_" } qw(src/core src/event src/event/modules
    src/os/unix objs src/http src/http/modules src/http/v2 src/http/v3);
my @commands = map {
    {directory => $nginx, file => "$root/$_.c",
     arguments => ['gcc', '-std=gnu89', @includes, '-c', "$root/$_.c"]}
} ($sources =~ /(ngx_cache_purge_\w+)\.c/g);
open my $db, '>', 'objs/purge-compile-commands.json' or die $!;
print {$db} encode_json(\@commands);
close $db or die $!;
# Keep style suggestions visible without making C89 declaration placement,
# core-style non-const API pointers or label/continue idioms release blockers.
# Unused inline helpers in upstream headers are outside this module's scope.
# Cppcheck can report their paths relative to the compilation directory.
open my $advisory, '>', 'objs/purge-cppcheck-advisory.txt' or die $!;
print {$advisory} "variableScope\nconstParameterPointer\nconstVariablePointer\n",
                  "redundantContinue\nunusedFunction:$nginx/src/*.h\n",
                  "unusedFunction:src/*.h\n";
close $advisory or die $!;
exec 'cppcheck', '--project=objs/purge-compile-commands.json', '--enable=all',
     '--inconclusive', '--std=c89', '--suppress=missingIncludeSystem',
     '--exitcode-suppressions=objs/purge-cppcheck-advisory.txt',
     '--error-exitcode=1';
die "cppcheck: $!";
