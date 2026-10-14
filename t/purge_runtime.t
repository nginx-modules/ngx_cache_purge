use strict;
use warnings;
use Test::More;
use Digest::MD5 qw(md5_hex);
use Compress::Raw::Zlib qw(crc32);
use Cwd qw(abs_path);
use File::Basename qw(dirname);
use File::Find qw(find);
use File::Path qw(make_path);
use File::Temp qw(tempdir);
use HTTP::Tiny;
use IO::Socket::INET;
use JSON::PP qw(decode_json);
use POSIX qw(setsid WNOHANG);
use Time::HiRes qw(time sleep);
use Text::ParseWords qw(shellwords);

my $nginx = $ENV{TEST_NGINX_BINARY} || 'nginx';
my $workers = $ENV{TEST_NGINX_WORKERS} || 8;
my $files = $ENV{TEST_NGINX_FILES} || 4096;
my $base = tempdir('purge-runtime-XXXXXX', TMPDIR => 1, CLEANUP => 1);
my $http = HTTP::Tiny->new(timeout => 3, keep_alive => 0, no_proxy => '127.0.0.1');
my @servers;
my $layout;

sub cache_header_layout {
    return $layout if $layout;
    my $source = $ENV{NGINX_SOURCE} or die 'NGINX_SOURCE is required';
    my $build = $ENV{NGINX_BUILD_DIR} || "$source/objs";
    my @inc = ("-I$build", map { "-I$source/$_" }
               qw(src/core src/event src/event/modules src/os/unix
                  src/http src/http/modules src/http/v2 src/http/v3));
    my $fixture = dirname(abs_path(__FILE__)) . '/fixtures/cache_header_layout.c';
    system(shellwords($ENV{CC} || 'cc'), @inc, $fixture, '-o', "$base/header-layout") == 0
        or die 'cannot compile native cache header layout';
    open my $pipe, '-|', "$base/header-layout" or die $!;
    local $/;
    $layout = decode_json(<$pipe>);
    close $pipe or die 'cannot read native cache header layout';
    return $layout;
}

sub read_file {
    my ($path) = @_;
    open my $fh, '<', $path or die "$path: $!";
    binmode $fh;
    local $/;
    return <$fh>;
}

sub write_file {
    my ($path, $data) = @_;
    open my $fh, '>', $path or die "$path: $!";
    binmode $fh;
    print {$fh} $data or die "$path: $!";
    close $fh or die "$path: $!";
}

sub await {
    my ($test, $why, $seconds) = @_;
    my $deadline = time + ($seconds || 10);
    while (time < $deadline) {
        return if $test->();
        sleep 0.01;
    }
    die "$why\n";
}

sub config {
    my ($s) = @_;
    my $user = $> == 0 ? 'user root;' : '';
    my $background = $s->{background} ? 'on' : 'off';
    my $legacy = exists $s->{legacy} ? "cache_purge_legacy_status $s->{legacy};" : '';
    my $response = $s->{default_response} ? '' : 'cache_purge_response_type json;';
    my $count = $s->{workers} || $workers;
    my $pool = $s->{threads} ? 'thread_pool cleanup threads=2 max_queue=1;' : '';
    my $selection = $s->{threads} ? 'cache_purge_thread_pool cleanup;' : '';
    my $key = $s->{strict} ? '$cache_purge_key' : '$uri';
    my $logical = $s->{logical_key} || '$uri';
    my $origin_port = $s->{origin_port} || $s->{port};
    my $profile_host = $s->{profile_host} || 'origin.test';
    my $refresh_timeout = $s->{refresh_timeout} || '10s';
    my $namespace = $s->{strict}
        ? "cache_purge_namespace objects 1m $s->{root}/purge-state;\n"
          . "cache_purge_key objects $logical;" : '';
    my $vary = $s->{strict} ? '' : 'add_header Vary Accept-Encoding always;';
    my $refresh = $s->{strict} ? <<"REFRESH" : '';
        location = /refresh {
            cache_purge_response_type none;
            cache_purge_key objects \$http_x_refresh_key;
            proxy_cache_refresh a \$cache_purge_key;
            cache_purge_refresh_profile /probe /cache;
            cache_purge_refresh_timeout $refresh_timeout;
            allow 127.0.0.1; deny all;
        }
        location = /bad-key {
            proxy_cache a;
            proxy_cache_key wrong-key;
            proxy_cache_purge PURGE from 127.0.0.1;
        }
        location = /probe {
            internal;
            proxy_cache_purge off;
            proxy_cache off;
            proxy_pass_request_headers off;
            proxy_pass_request_body off;
            proxy_connect_timeout 500ms;
            proxy_send_timeout 1s;
            proxy_read_timeout 1s;
            proxy_method HEAD;
            proxy_set_header Host $profile_host;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match \$cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since \$cache_purge_refresh_last_modified;
            proxy_pass http://127.0.0.1:$origin_port/origin\$cache_purge_refresh_uri;
        }
REFRESH
    my $responses = join "\n", map {
        "location /response/$_ { proxy_cache a; "
        . 'proxy_cache_key $http_x_purge_key; '
        . 'proxy_cache_purge PURGE purge_all from 127.0.0.1; '
        . "cache_purge_response_type $_; }"
    } qw(html json xml text none);
    return <<EOF;
$user
daemon off;
worker_processes $count;
$pool
pid $s->{root}/nginx.pid;
error_log $s->{root}/error.log notice;
events { worker_connections 1024; accept_mutex off; }
http {
    access_log off;
    $selection
    $namespace
    client_body_temp_path $s->{root}/client-temp;
    proxy_temp_path $s->{temp};
    proxy_cache_path $s->{cache} levels=1:2 keys_zone=a:2m inactive=1h;
    proxy_cache_path $s->{cache_b} levels=1:2 keys_zone=b:2m inactive=1h;
    cache_purge_background_queue $background;
    cache_purge_queue_size $s->{capacity};
    cache_purge_batch_size $s->{batch};
    cache_purge_throttle_ms $s->{throttle};
    cache_purge_vary_aware on;
    $response
    $legacy
    server {
        listen 127.0.0.1:$s->{port};
        root $s->{root};
        $responses
        $refresh
        proxy_cache_purge PURGE from 127.0.0.1;
        location /health { proxy_cache_purge off; }
        location /origin/ {
            proxy_cache_purge off;
            $vary
        }
        location /cache/ {
            proxy_pass http://127.0.0.1:$origin_port/origin/;
            proxy_set_header Host origin.test;
            proxy_cache a;
            proxy_cache_key $key;
            proxy_cache_valid 200 1h;
            add_header X-Cache \$upstream_cache_status always;
        }
        location /missing-cache {
            proxy_cache_purge PURGE from 127.0.0.1;
        }
        location /all {
            proxy_cache a;
            proxy_cache_key $key;
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
        location /denied {
            proxy_cache a;
            proxy_cache_key $key;
            proxy_cache_purge PURGE from 10.255.255.1;
        }
        location /status-a {
            cache_purge_response_type none;
            cache_purge_queue_status $s->{cache}///;
            add_header X-Generation $s->{generation} always;
            add_header X-Queue-Config $background always;
            location /status-a/child { }
            location /status-a/purge {
                proxy_cache a;
                proxy_cache_key $key;
                proxy_cache_purge PURGE from 127.0.0.1;
            }
        }
        location /status-b { cache_purge_queue_status $s->{cache_b}; }
    }
}
EOF
}

sub request {
    my ($s, $uri, $method, $headers) = @_;
    return $http->request($method || 'GET',
                         "http://127.0.0.1:$s->{port}$uri",
                         { headers => { 'Accept-Encoding' => 'identity',
                                        %{ $headers || {} } } });
}

sub status_fields {
    my ($value, $shape, $path) = @_;
    if (ref $shape eq 'HASH') {
        die "invalid $path" unless ref $value eq 'HASH'
            && !grep { !exists $value->{$_} } keys %$shape;
        status_fields($value->{$_}, $shape->{$_}, "$path.$_") for keys %$shape;
    } elsif (JSON::PP::is_bool($shape)) {
        die "invalid $path" unless JSON::PP::is_bool($value);
    } elsif ($shape eq 'ratio') {
        die "invalid $path" if !defined $value || ref $value
            || $value !~ /^\d+(?:\.\d+)?$/ || $value > 1;
    } else {
        die "invalid $path" if !defined $value || ref $value || $value !~ /^\d+$/;
    }
}

sub status {
    my ($s, $uri) = @_;
    my $r = request($s, $uri || '/status-a');
    die "status response: $r->{status} $r->{content}" if $r->{status} != 200;
    die 'cacheable status' if $r->{headers}{'cache-control'} ne 'no-store';
    die 'status media type' if $r->{headers}{'content-type'} ne 'application/json';
    die 'status length' if $r->{headers}{'content-length'} != length $r->{content};
    my $v = decode_json($r->{content});
    status_fields($v, {
        queue => { size => 0, running => 0, max_size => 0, wait_time => 0,
                   full => JSON::PP::false },
        cache => { pending => { total => 0, full => 0 }, active => 0,
                   requests => { overflow => 0, deduplicated => 0, waiting => 0 },
                   files => { deleted => 0, errors => 0, bytes => 0 },
                   cleanup => { scan_errors => 0, retries => 0, exhausted => 0 },
                   logical => { invalidations => 0 },
                   responses => { hit => 0, miss => 0, hit_ratio => 'ratio' } }
    }, 'status');
    # Fields are approximate aligned loads, not a cross-worker transaction.
    # Admission uses locked control state, never these monitoring snapshots.
    return $v;
}

sub drain {
    my ($s) = @_;
    await(sub { my $v = status($s); !$v->{cache}{pending}{total} && !$v->{cache}{active} },
          'pending work did not drain', 30);
}

# A controllable origin gives deterministic late-writer/probe races. It runs
# only as part of the container runtime suite (native macOS is the CI exception).
sub start_origin {
    my ($s) = @_;
    my $listener = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => 0,
                                       Listen => 64, ReuseAddr => 1) or die $!;
    $s->{origin_port} = $listener->sockport;
    write_file("$s->{root}/version", '1');
    my $pid = fork();
    die "origin fork: $!" unless defined $pid;
    if (!$pid) {
        setsid() >= 0 or POSIX::_exit(127);
        $SIG{CHLD} = 'IGNORE';
        $SIG{PIPE} = 'IGNORE';
        while (my $client = $listener->accept()) {
            my $child = fork();
            if (!defined $child) { close $client; next; }
            if ($child) { close $client; next; }
            close $listener;
            $client->autoflush(1);
            my $request = '';
            while (my $line = <$client>) {
                $request .= $line;
                last if $line eq "\r\n";
            }
            my $head = $request =~ /^HEAD /;
            my $version = read_file("$s->{root}/version");
            my $kind = $head ? 'head' : 'get';
            if (-f "$s->{root}/$kind-block") {
                write_file("$s->{root}/$kind-started", $request);
                my $until = time + 5;
                sleep 0.005 until -f "$s->{root}/$kind-release" || time >= $until;
            }
            my $status = $head && -f "$s->{root}/head-status"
                         ? read_file("$s->{root}/head-status")
                         : $head && $request =~ /If-None-Match: "v\Q$version\E"/i
                           ? 304 : 200;
            if ($status) {
                my $body = "object version $version\n";
                my $vary = -f "$s->{root}/vary" ? "Vary: Accept-Encoding\r\n" : '';
                print {$client} "HTTP/1.1 $status Test\r\nConnection: close\r\n"
                    . "ETag: \"v$version\"\r\nCache-Control: max-age=3600\r\n$vary"
                    . 'Content-Length: ' . length($body) . "\r\n\r\n"
                    . ($head ? '' : $body);
            }
            close $client;
            POSIX::_exit(0);
        }
        POSIX::_exit(0);
    }
    close $listener;
    $s->{origin_pid} = $pid;
}

sub stop_origin {
    my ($s) = @_;
    if (my $pid = delete $s->{origin_pid}) {
        kill 'TERM', -$pid;
        waitpid($pid, 0);
    }
}

sub pending_request {
    my ($s, $uri, $method, $key) = @_;
    my $socket = IO::Socket::INET->new(PeerAddr => '127.0.0.1',
                     PeerPort => $s->{port}, Timeout => 5) or die $!;
    $socket->autoflush(1);
    print {$socket} "$method $uri HTTP/1.0\r\nHost: localhost\r\n"
        . (defined $key ? "X-Refresh-Key: $key\r\n" : '') . "\r\n";
    return $socket;
}

sub finish_request {
    my ($socket) = @_;
    local $/;
    my $response = <$socket>;
    close $socket;
    die 'missing asynchronous response' unless defined $response;
    return $response;
}

sub start_server {
    my ($name, %options) = @_;
    my $s = { root => "$base/$name", generation => 0, background => 1,
              capacity => 100, batch => 10, throttle => '10ms', stopped => [],
              threads => $ENV{TEST_NGINX_THREADS}, %options };
    $s->{cache} = "$s->{root}/proxy_temp/cache-a";
    $s->{cache_b} = "$s->{root}/cache-b";
    $s->{temp} = "$s->{cache}/custom-spool";
    my $socket = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => 0,
                                     Listen => 1) or die $!;
    $s->{port} = $socket->sockport;
    close $socket;
    if ($s->{linked_root}) {
        make_path(dirname($s->{cache}), "$s->{cache}.real");
        symlink "$s->{cache}.real", $s->{cache} or die $!;
    }
    make_path($s->{cache}, $s->{cache_b}, "$s->{root}/origin", "$s->{root}/logs");
    write_file("$s->{root}/health", 'ready');
    write_file("$s->{root}/origin/$_", "cache fixture $_") for qw(warm item);
    start_origin($s) if $s->{controlled_origin};
    write_file("$s->{root}/nginx.conf", config($s));
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        setsid() >= 0 or die "setsid: $!";
        open STDOUT, '>', "$s->{root}/process.log" or die $!;
        open STDERR, '>&', \*STDOUT or die $!;
        exec $nginx, '-g', '', '-p', "$s->{root}/",
             '-c', "$s->{root}/nginx.conf" or POSIX::_exit(127);
    }
    $s->{pid} = $pid;
    push @servers, $s;
    await(sub {
        if (waitpid($pid, WNOHANG) == $pid) {
            delete $s->{pid};
            die read_file("$s->{root}/process.log");
        }
        request($s, '/health')->{status} == 200;
    }, 'nginx did not start');
    return $s;
}

sub stop_server {
    my ($s) = @_;
    return unless my $pid = delete $s->{pid};
    kill 'CONT', @{$s->{stopped}} if @{$s->{stopped}};
    kill 'QUIT', $pid;
    my $rc;
    my $ok = eval {
        await(sub { return 0 unless waitpid($pid, WNOHANG) == $pid;
                    $rc = $?; return 1; }, 'graceful exit stalled', 15);
        1;
    };
    if (!$ok) { kill 'TERM', -$pid; waitpid($pid, 0); die $@; }
    stop_origin($s);
    is($rc, 0, 'master exited cleanly');
    my $log = read_file("$s->{root}/error.log");
    unlike($log, qr/exited on signal/, 'no worker crash');
    my $report = read_file("$s->{root}/process.log");
    unlike($report . $log, qr/AddressSanitizer|runtime error:/,
           'no sanitizer report');
    my @severe = grep { /\[(?:crit|alert|emerg)\]/ } split /\n/, $log;
    @severe = grep {
        !/\[crit\].*stat\(\) "\Q$s->{cache}\E\/.*" failed \(2: No such file or directory\)/
    } @severe;
    if ($ENV{TEST_NGINX_ALLOW_INITGROUPS_WARNING}) {
        @severe = grep { !/\[emerg\].*initgroups\(root, 0\) failed \(1: Operation not permitted\)$/ } @severe;
    }
    if ($s->{resize_rejected}) {
        @severe = grep { !/\[emerg\].*cache_purge_queue_size changes/ } @severe;
    }
    is_deeply(\@severe, [], 'no unexpected severe diagnostic');

}

END {
    for my $s (@servers) {
        next unless $s->{pid};
        kill 'CONT', @{$s->{stopped}} if @{$s->{stopped}};
        kill 'TERM', -$s->{pid};
        waitpid($s->{pid}, 0);
        stop_origin($s);
    }
}

sub warm {
    my ($s) = @_;
    await(sub { (request($s, '/cache/warm')->{headers}{'x-cache'} || '') eq 'HIT' },
          'cache did not warm');
    my $path;
    find(sub { $path ||= $File::Find::name if -f $_ && /^[0-9a-f]{32}$/ }, abs_path($s->{cache}));
    my $bytes = read_file($path);
    my $offset = index($bytes, "\nKEY: ");
    die 'missing cache header' if $offset < 0;
    return substr($bytes, 0, $offset + 6);
}

sub fixture {
    my ($s, $key, $prefix, $directory) = @_;
    my $digest = md5_hex($key);
    $directory ||= "$s->{cache}/" . substr($digest, -1) . '/' . substr($digest, -3, 2);
    make_path($directory);
    my $path = "$directory/$digest";
    my $native = cache_header_layout();
    die 'fixture/header layout mismatch' unless length($prefix) == $native->{size} + 6;
    my %value = (crc32 => crc32($key), header_start => length($prefix) + length($key) + 1,
                 body_start => length($prefix) + length($key) + 1, etag_len => 0, vary_len => 0);
    for my $field (keys %value) {
        my ($offset, $size) = @{$native->{fields}{$field}};
        my $template = {1 => 'C', 2 => 'S', 4 => 'L', 8 => 'Q'}->{$size}
            or die "unsupported native field width: $size";
        substr($prefix, $offset, $size) = pack($template, $value{$field});
    }
    write_file($path, "$prefix$key\nbody\n");
    return $path;
}

sub reload_server {
    my ($s) = @_;
    $s->{generation}++;
    write_file("$s->{root}/nginx.conf", config($s));
    kill 'HUP', $s->{pid};
    await(sub { (request($s, '/status-a')->{headers}{'x-generation'} || '-1')
                eq $s->{generation} }, 'new generation did not serve a response');
}

sub check_config {
    my ($s, $text) = @_;
    write_file("$s->{root}/invalid.conf", $text);
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        open STDOUT, '>', "$s->{root}/config.log" or die $!;
        open STDERR, '>&', \*STDOUT or die $!;
        exec $nginx, '-g', '', '-t', '-p', "$s->{root}/",
             '-c', "$s->{root}/invalid.conf" or POSIX::_exit(127);
    }
    waitpid($pid, 0) == $pid or die "waitpid: $!";
    my $rc = $?;
    my $log = read_file("$s->{root}/config.log");
    unlike($log, qr/AddressSanitizer|LeakSanitizer|runtime error:/,
           'configuration check has no sanitizer report') or diag($log);
    return $rc;
}

subtest 'response escaping and content lengths' => sub {
    for my $background (0, 1) {
        my $s = start_server("responses-$background", background => $background);
        for my $key ('', q{<script>"quoted" & \path ]]> ]]><tag>}) {
        for my $type (qw(html json xml text)) {
            my $r = request($s, "/response/$type", 'PURGE', { 'X-Purge-Key' => $key });
            is($r->{status}, $background ? 202 : 200, "$type status");
            is($r->{headers}{'content-length'}, length $r->{content}, "$type length");
            my $state = $background ? 'accepted' : 'no_match';
            is($r->{headers}{'cache-purge-code'}, $state, 'stable header code');
            is($r->{headers}{'cache-control'}, 'no-store', 'administrative response is not cacheable');
            if ($type eq 'json') {
                my $body = decode_json($r->{content});
                is($body->{key}, $key, 'JSON preserves the decoded key');
                is($body->{code}, $state, 'JSON and header codes agree');
                is($body->{complete}, $background ? 0 : 1, 'completion is explicit');
                is($body->{cleanup_pending}, $background ? 1 : 0, 'cleanup state is explicit');
                is($body->{deleted}, 0, 'empty synchronous scan has zero deletions') unless $background;
            } elsif ($type eq 'html') {
                unlike($r->{content}, qr/<script>|<tag>/, 'HTML key cannot add tags');
                like($r->{content}, $key ? qr/&lt;script&gt;&quot;quoted&quot; &amp;/
                                        : qr/<p>Key: <\/p>/,
                     'HTML key is escaped');
            } elsif ($type eq 'xml') {
                my ($encoded) = $r->{content} =~ m{<Key>(.*?)</Key>};
                my $decoded = join '', $encoded =~ /<!\[CDATA\[(.*?)\]\]>/g;
                is($decoded, $key, 'CDATA terminators preserve the decoded key');
            } else {
                is($r->{content}, "Key: $key\nCode: $state\nKey encoding: utf-8\n", 'text key is unchanged');
            }
        }
        }
        stop_server($s);
    }
};

sub cached_entries {
    my ($s) = @_;
    my @entries;
    find(sub {
        return unless -f $_ && /^[0-9a-f]{32}$/;
        my $data = read_file($File::Find::name);
        my ($key) = $data =~ /\nKEY: ([^\n]*)\n/;
        push @entries, [$File::Find::name, $key, $data] if defined $key;
    }, abs_path($s->{cache}));
    return @entries;
}

sub refresh {
    my ($s, $key, $cursor) = @_;
    my %headers = ('X-Refresh-Key' => $key);
    $headers{'X-Cache-Purge-Cursor'} = $cursor if defined $cursor;
    my $r = request($s, '/refresh', 'REFRESH', \%headers);
    is($r->{status}, 200, 'REFRESH returns its explicit result document');
    my $body = decode_json($r->{content});
    is($r->{headers}{'cache-purge-code'}, $body->{code},
       'REFRESH header and body codes agree');
    is($body->{code}, $body->{complete} ? 'refresh_complete' : 'refresh_incomplete',
       'REFRESH code describes scan completion');
    return $body;
}

subtest 'HTTP result contract and legacy directive migration' => sub {
    for my $legacy ('on', 'off') {
        my $s = start_server("contract-$legacy", workers => 1, background => 0,
                             legacy => $legacy, default_response => 1);
        for my $case (['/cache/absent', 'PURGE', 200, 'no_match'],
                      ['/cache/absent*', 'PURGE', 200, 'no_match'],
                      ['/denied/x', 'PURGE', 403, 'access_denied'],
                      ['/all', 'POST', 405, 'method_not_allowed'],
                      ['/missing-cache', 'PURGE', 500, 'cache_unavailable'],
                      ['/status-a', 'POST', 405, 'method_not_allowed']) {
            my ($uri, $method, $status, $code) = @$case;
            my $r = request($s, $uri, $method);
            is($r->{status}, $status, "$legacy: $code status");
            is($r->{headers}{'cache-purge-code'}, $code, "$legacy: header code");
            my $body = decode_json($r->{content});
            is($body->{code}, $code, "$legacy: body code");
            if ($status >= 400) {
                is($r->{headers}{'content-type'}, 'application/problem+json', 'problem media type');
                is($body->{status}, $status, 'problem status matches HTTP');
                is($body->{type}, 'about:blank', 'standard generic problem type');
                ok($body->{title} && $body->{detail}, 'human-readable problem fields');
            } else {
                is($r->{headers}{'content-type'}, 'application/json', 'JSON is the default');
            }
            is($r->{headers}{'allow'}, $uri eq '/status-a' ? 'GET, HEAD' : 'PURGE',
               '405 identifies supported methods') if $status == 405;
        }
        my $none = request($s, '/response/none', 'PURGE');
        is($none->{status}, 204, 'explicit bodyless completion');
        is($none->{content} // '', '', '204 has no body');
        is($none->{headers}{'cache-purge-code'}, 'no_match', 'bodyless response retains result');
        my $none_error = request($s, '/response/none', 'GET');
        is($none_error->{status}, 405, 'bodyless error retains HTTP status');
        is($none_error->{content} // '', '', 'bodyless error omits problem body');
        is($none_error->{headers}{'cache-purge-code'}, 'method_not_allowed',
           'bodyless error retains result code');
        my $socket = IO::Socket::INET->new(PeerAddr => '127.0.0.1',
                         PeerPort => $s->{port}, Timeout => 5) or die $!;
        $socket->autoflush(1);
        print {$socket} "PURGE /response/json HTTP/1.0\r\nHost: localhost\r\nX-Purge-Key: \xff\r\n\r\n";
        my $wire = finish_request($socket);
        my (undef, $json) = split /\r\n\r\n/, $wire, 2;
        my $body = decode_json($json);
        is($body->{key_encoding}, 'base64', 'binary key has an explicit encoding');
        is($body->{key}, '/w==', 'binary key is represented losslessly');
        like(read_file("$s->{root}/process.log") . read_file("$s->{root}/error.log"), qr/cache_purge_legacy_status is obsolete and ignored/,
             'legacy setting emits a configuration warning');
        stop_server($s);
    }
};

subtest 'strict generation invalidation precedes deferred physical cleanup' => sub {
    my $s = start_server('strict', strict => 1, throttle => '60s');
    my $bad = request($s, '/bad-key', 'PURGE');
    is($bad->{status}, 500, 'namespace key mismatch is a server error');
    is($bad->{headers}{'cache-purge-code'}, 'key_mismatch', 'key mismatch header code');
    is(decode_json($bad->{content})->{code}, 'key_mismatch', 'key mismatch problem code');
    my $long = request($s, '/refresh', 'REFRESH', { 'X-Refresh-Key' => 'x' x 8097 });
    is($long->{status}, 400, 'oversized namespace selector is a client error');
    is($long->{headers}{'cache-purge-code'}, 'invalid_selector', 'selector header code');
    is(decode_json($long->{content})->{code}, 'invalid_selector', 'selector problem code');
    warm($s);
    my ($old) = grep { $_->[1] =~ m{/cache/warm$} } cached_entries($s);
    ok($old, 'native cache key includes the namespace');
    my $r = request($s, '/cache/warm', 'PURGE');
    is($r->{status}, 200, 'logical invalidation returns 200');
    is(decode_json($r->{content})->{code}, 'invalidated', 'response states logical completion');
    ok(-e $old->[0], 'old file still exists while the executor is delayed');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'new request cannot hit old generation');
    await(sub { (request($s, '/cache/warm')->{headers}{'x-cache'} || '') eq 'HIT' }, 'new generation did not warm');
    my @entries = grep { $_->[1] =~ m{/cache/warm$} } cached_entries($s);
    is(scalar @entries, 2, 'new generation has a different physical filename');
    is(request($s, '/cache/*', 'PURGE')->{status}, 200, 'prefix generation invalidated');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'prefix invalidation affects exact lookups');
    is(request($s, '/all', 'PURGE')->{status}, 200, 'namespace baseline invalidated');
    my $before = status($s)->{cache}{logical}{invalidations};
    is($before, 3, 'logical invalidations have a separate metric');
    reload_server($s);
    is(status($s)->{cache}{logical}{invalidations}, $before, 'reload preserves namespace and counters');
    stop_server($s);
    $s = start_server('strict', strict => 1, throttle => '60s');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'full restart never reuses a persisted epoch');
    stop_server($s);
};

subtest 'REFRESH conditional decisions and bounded continuation' => sub {
    my $s = start_server('refresh', strict => 1);
    warm($s);
    is(request($s, '/cache/warm', 'REFRESH')->{status}, 405,
       'legacy PURGE capability does not accept REFRESH');
    is(request($s, '/refresh', 'PURGE')->{status}, 405, 'new endpoint restricts its method');
    my $r = refresh($s, '/cache/warm');
    ok($r->{scan_complete} && $r->{complete} && !$r->{timed_out}, 'single-entry scan completed');
    is($r->{results}[0]{upstream_status}, 304, 'raw conditional origin status');
    is($r->{results}[0]{result}, 'kept', '304 retains the entry');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'HIT', '304 does not invalidate');
    write_file("$s->{root}/origin/warm", 'changed origin with a distinct entity length');
    $r = refresh($s, '/cache/warm');
    is($r->{results}[0]{upstream_status}, 200, 'changed origin responds 200');
    is($r->{results}[0]{result}, 'invalidated', '200 publishes a new generation');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'REFRESH invalidation is visible before unlink');
    await(sub { (request($s, '/cache/warm')->{headers}{'x-cache'} || '') eq 'HIT' }, 'entry did not refill');
    unlink "$s->{root}/origin/warm" or die $!;
    $r = refresh($s, '/cache/warm');
    is($r->{results}[0]{upstream_status}, 404, 'missing origin responds 404');
    is($r->{results}[0]{result}, 'invalidated', '404 invalidates');
    for (0 .. 36) {
        write_file("$s->{root}/origin/page$_", "page $_");
        await(sub { (request($s, "/cache/page$_")->{headers}{'x-cache'} || '') eq 'HIT' }, 'page entry did not warm');
    }
    my (%seen, $cursor, $pages);
    do {
        $r = refresh($s, '/cache/page*', $cursor);
        ok(@{$r->{results}} <= 32, 'HTTP request owns at most 32 probes');
        for my $result (@{$r->{results}}) {
            ok(!$seen{$result->{hash}}++, 'continuation does not repeat a kept entry');
            is($result->{result}, 'kept', 'conditional page entry retained');
        }
        $cursor = $r->{next_cursor};
        die 'continuation failed to finish' if ++$pages > 3;
    } until $r->{complete};
    is(scalar keys %seen, 37, 'all matching entries visited through bounded pages');
    stop_server($s);
};

subtest 'late writers and late REFRESH cannot invalidate replacement generations' => sub {
    my $s = start_server('races', strict => 1, controlled_origin => 1);
    write_file("$s->{root}/get-block", '');
    my $get = pending_request($s, '/cache/late', 'GET');
    await(sub { -f "$s->{root}/get-started" }, 'old generation GET did not reach origin');
    is(request($s, '/cache/late', 'PURGE')->{status}, 200, 'PURGE accepts an in-flight cold key');
    write_file("$s->{root}/version", '2');
    unlink "$s->{root}/get-block" or die $!;
    write_file("$s->{root}/get-release", '');
    like(finish_request($get), qr/object version 1/, 'already-started response may finish with old data');
    my $next = request($s, '/cache/late');
    is($next->{headers}{'x-cache'}, 'MISS', 'late old write is unreachable by a new GET');
    like($next->{content}, qr/object version 2/, 'new generation fills from new origin data');
    await(sub { (request($s, '/cache/late')->{headers}{'x-cache'} || '') eq 'HIT' }, 'replacement did not warm');
    write_file("$s->{root}/head-block", '');
    write_file("$s->{root}/head-status", '200');
    my $probe = pending_request($s, '/refresh', 'REFRESH', '/cache/late');
    await(sub { -f "$s->{root}/head-started" }, 'conditional probe did not reach origin');
    is(request($s, '/cache/late', 'PURGE')->{status}, 200, 'PURGE races with an active HEAD');
    request($s, '/cache/late');
    write_file("$s->{root}/head-release", '');
    my (undef, $body) = split /\r\n\r\n/, finish_request($probe), 2;
    is(decode_json($body)->{results}[0]{result}, 'raced_replaced', 'old HEAD cannot invalidate a replacement');
    is(request($s, '/cache/late')->{headers}{'x-cache'}, 'HIT', 'replacement is still reachable');
    stop_server($s);
};

subtest 'REFRESH failure policy, profile isolation, URI identity and deadlines' => sub {
    my $s = start_server('probe-errors', strict => 1, controlled_origin => 1,
                         logical_key => '$request_uri');
    my $key = '/cache/query?part=a%2Fb&other=1';
    await(sub { (request($s, $key)->{headers}{'x-cache'} || '') eq 'HIT' }, 'query key did not warm');
    write_file("$s->{root}/head-block", '');
    write_file("$s->{root}/head-release", '');
    my $r = refresh($s, $key);
    is($r->{results}[0]{result}, 'kept', 'query representation receives a conditional 304');
    my $head = read_file("$s->{root}/head-started");
    like($head, qr{^HEAD /origin/query\?part=a%2Fb&other=1 HTTP/}, 'escaped URI and query survive the explicit mapping');
    like($head, qr/\r\nHost: origin\.test\r\n/i, 'literal profile Host is used');
    unlike($head, qr/\r\n(?:Authorization|Cookie):/i, 'administrative credentials are not forwarded');
    for my $status (500, 0, 410) {
        write_file("$s->{root}/head-status", "$status");
        $r = refresh($s, $key);
        is($r->{results}[0]{upstream_status}, $status, 'raw origin status remains distinct from local errors');
        is($r->{results}[0]{result}, $status == 410 ? 'invalidated'
                                   : $status ? 'upstream_status' : 'transport_error',
           'explicit result for origin or transport failure');
    }
    request($s, $key);
    $s->{profile_host} = '$http_host';
    reload_server($s);
    $r = refresh($s, $key);
    is($r->{results}[0]{result}, 'skipped_profile', 'client-controlled Host profile is rejected');
    like(read_file("$s->{root}/error.log"),
         qr/\[error\].*ngx_cache_purge: REFRESH profile "[^"]+" rejected: header "Host" must have a literal value/,
         'REFRESH diagnostic identifies the profile and the rejected header');
    $s->{profile_host} = 'origin.test';
    $s->{refresh_timeout} = '20ms';
    reload_server($s);
    unlink "$s->{root}/head-release" or die $!;
    write_file("$s->{root}/head-status", '200');
    $r = refresh($s, $key);
    ok($r->{timed_out} && !$r->{complete}, 'deadline reports incomplete work');
    is($r->{next_cursor}, '0' x 32, 'unfinished probe does not advance the continuation');
    write_file("$s->{root}/head-release", '');
    is(request($s, $key)->{headers}{'x-cache'}, 'HIT', 'timed-out decision retains the current generation');
    stop_server($s);

    $s = start_server('vary-profile', strict => 1, controlled_origin => 1);
    write_file("$s->{root}/vary", '');
    await(sub { (request($s, '/cache/vary')->{headers}{'x-cache'} || '') eq 'HIT' }, 'Vary entry did not warm');
    $r = refresh($s, '/cache/vary');
    is($r->{results}[0]{result}, 'skipped_vary', 'single-representation profile never guesses a Vary request');
    is(request($s, '/cache/vary')->{headers}{'x-cache'}, 'HIT', 'skipped Vary entry is preserved');
    stop_server($s);
};

subtest 'PR72: every walk skips descendant links and accepts a linked root' => sub {
    for my $background (0, 1) {
        my $s = start_server("links-$background", background => $background, linked_root => 1);
        my $prefix = warm($s);
        my $outside = fixture($s, '/cache/item', $prefix, "$s->{root}/outside");
        my $before = read_file($outside);
        symlink "$s->{root}/outside", "$s->{cache}/linked-directory" or die $!;
        for my $operation ('/all', '/cache/*', '/cache/item') {
            await(sub { (request($s, '/cache/item')->{headers}{'x-cache'} || '') eq 'HIT' },
                  'genuine cache file did not warm');
            my $r = request($s, $operation, 'PURGE');
            is($r->{status}, $background && $operation ne '/cache/item' ? 202 : 200,
               "linked-root purge: $operation");
            drain($s) if $background;
            is(read_file($outside), $before, 'valid hash-named outside cache file is unchanged');
            is(request($s, '/cache/item')->{headers}{'x-cache'}, 'MISS',
               'real entry below the configured root is removed');
        }
        stop_server($s);
    }
};

subtest 'path protection, concurrent walks and diagnostics' => sub {
    my $s = start_server('walks');
    is(status($s)->{cache}{pending}{total}, 0, 'empty initially');
    is(request($s, '/status-a', 'POST')->{status}, 405, 'method restriction');
    my $head = request($s, '/status-a', 'HEAD');
    ok($head->{status} == 200 && !$head->{content}
       && $head->{headers}{'content-length'} > 0, 'HEAD has no body');
    is(request($s, '/status-a/purge', 'PURGE')->{status}, 204,
       'child purge handler inherits bodyless format');
    is(request($s, '/denied/x', 'PURGE')->{status}, 403, 'access rejection');
    is(request($s, '/cache/' . ('x' x 600) . '*', 'PURGE')->{status}, 400,
       'long prefix rejected');
    my $prefix = warm($s);
    my @protected = (fixture($s, '/cache/batch0/protected', $prefix, "$s->{temp}/nested"));
    for my $name ('0000000123', ('a' x 32) . '.0000000123') {
        my $path = "$s->{cache}/$name";
        write_file($path, 'in-flight response');
        push @protected, $path;
    }
    my %bytes = map { $_ => read_file($_) } @protected;
    make_path("$s->{root}/outside");
    my $outside = fixture($s, '/cache/external', $prefix, "$s->{root}/outside");
    symlink "$s->{root}/outside", "$s->{cache}/linked-directory" or die $!;
    symlink $outside, "$s->{cache}/" . md5_hex('/cache/linked-file') or die $!;
    my @ordinary = map { fixture($s, '/cache/plain', $prefix, "$s->{cache}/$_") }
                       qw(main/app_proxy_temp_file app/proxy_temp custom-spool-backup);
    is(request($s, '/all', 'PURGE')->{status}, 202, 'purge-all accepted');
    drain($s);
    ok(!grep({ -e $_ } @ordinary), 'unconfigured and similar directory names deleted');
    ok(-e $outside, 'directory and file symlinks are not followed');
    is(status($s)->{cache}{files}{deleted}, 4, 'completed deletion total includes warm entry');
    my @targets = map { fixture($s, '/cache/batch' . ($_ % 4) . "/entry$_", $prefix) }
                      0 .. $files - 1;
    push @targets, fixture($s, '/cache/BATCH0/entry', $prefix);
    my @children;
    for my $batch (0 .. 3) {
        my $pid = fork();
        die $! unless defined $pid;
        if (!$pid) {
            my $r = request($s, "/cache/batch$batch*", 'PURGE');
            POSIX::_exit($r->{status} == 202 ? 0 : 1);
        }
        push @children, $pid;
    }
    for (@children) { waitpid($_, 0); is($?, 0, 'concurrent task accepted'); }
    drain($s);
    ok(!grep({ -e $_ } @targets), 'all wildcard targets deleted');
    is(read_file($_), $bytes{$_}, "protected file preserved: $_") for @protected;
    for my $encoding (qw(identity gzip)) {
        await(sub {
            (request($s, '/cache/item', 'GET', { 'Accept-Encoding' => $encoding })
             ->{headers}{'x-cache'} || '') eq 'HIT';
        }, "Vary fixture did not warm: $encoding");
    }
    my @distinct = map { fixture($s, $_, $prefix) }
                       ('/cache/ITEM', '/cache/itemExtra');
    is(request($s, '/cache/item', 'PURGE')->{status}, 200, 'exact Vary purge');
    ok(!grep({ !-e $_ } @distinct), 'exact match preserves case and key boundaries');
    for my $encoding (qw(identity gzip)) {
        is(request($s, '/cache/item', 'GET', { 'Accept-Encoding' => $encoding })
           ->{headers}{'x-cache'}, 'MISS', 'Vary entry invalidated');
    }
    my $log = read_file("$s->{root}/error.log");
    like($log, qr/\[error\].*ngx_cache_purge: access denied for 127\.0\.0\.1/,
         'access diagnostic retains level and client address');
    is(scalar(() = $log =~ /selector of \d+ bytes exceeds the supported limit, not purging/g),
       1, 'one long-prefix diagnostic');
    stop_server($s);
};

for my $pool ($ENV{TEST_NGINX_THREADS} ? (1) : (0)) {
subtest "batch limit survives traversal continuations, thread pool=$pool" => sub {
    my $s = start_server("batch-$pool", workers => 1, threads => $pool,
                         batch => 2, throttle => '60s');
    my $prefix = warm($s);
    for (1 .. 128) {
        fixture($s, "/cache/keep/$_", $prefix, "$s->{cache}/nonmatching/$_");
    }
    my @targets = map { fixture($s, "/cache/task$_/entry", $prefix) } 1 .. 3;
    for (1 .. 3) {
        is(request($s, "/cache/task$_/*", 'PURGE')->{status}, 202, 'task accepted');
    }
    is(status($s)->{cache}{pending}{total}, 3, 'three tasks await processing');
    $s->{throttle} = '1s';
    reload_server($s);
    my $v;
    await(sub { $v = status($s); $v->{cache}{files}{deleted} == 2 },
          'batch did not stop after two completed tasks');
    is($v->{cache}{pending}{total}, 1, 'third task waits for the next batch');
    is($v->{cache}{active}, 0, 'no active task during the batch pause');
    drain($s);
    is(status($s)->{cache}{files}{deleted}, 3, 'next batch completes the remaining task');
    ok(!grep({ -e $_ } @targets), 'all tasks completed after the batch pause');
    stop_server($s);
};

subtest "one worker serves HTTP and completes active work, thread pool=$pool" => sub {
    my $s = start_server("incremental-$pool", workers => 1, threads => $pool,
                         throttle => '1');
    my $prefix = warm($s);
    my @targets = map { fixture($s, "/cache/resume/$_", $prefix, $s->{cache}) }
                      1 .. $files;
    for (1 .. 128) {
        fixture($s, "/cache/keep/$_", $prefix, "$s->{cache}/nonmatching/$_");
    }
    is(request($s, '/cache/resume*', 'PURGE')->{status}, 202, 'work accepted');
    await(sub { status($s)->{cache}{active} },
          'single worker did not expose an active walk between events', 4);
    is(request($s, '/health')->{status}, 200, 'same worker serves HTTP during walk');
    my ($old) = read_file("$s->{root}/error.log") =~ /start worker process (\d+)/;
    reload_server($s);
    drain($s);
    ok(!grep({ -e $_ } @targets), 'active walk completed across reload');
    is(status($s)->{cache}{files}{deleted}, $files, 'active walk counted exactly once');
    await(sub { read_file("$s->{root}/error.log") =~ /worker process $old exited with code 0/ },
          'old worker did not finish graceful exit');
    like(read_file("$s->{root}/error.log"), qr/interpreting it as milliseconds/,
         'bare milliseconds keep their configuration warning');
    @targets = map { fixture($s, "/cache/terminate/$_", $prefix, $s->{cache}) }
                   1 .. $files;
    is(request($s, '/cache/terminate*', 'PURGE')->{status}, 202, 'exit work accepted');
    await(sub { status($s)->{cache}{active} }, 'exit walk never started', 4);
    my @running = read_file("$s->{root}/error.log") =~ /start worker process (\d+)/g;
    kill 'TERM', $running[-1] or die $!;
    eval {
        await(sub { read_file("$s->{root}/error.log")
                    =~ /worker process $running[-1] exited with code 0/ },
              'worker did not finish exit cleanup');
        1;
    } or do {
        my $error = $@;
        diag(read_file("$s->{root}/process.log"));
        diag(read_file("$s->{root}/error.log"));
        die $error;
    };
    await(sub { my $v = status($s); !$v->{cache}{active} && $v->{cache}{files}{deleted} == 2 * $files },
          'exit cleanup did not record completed work');
    ok(!grep({ -e $_ } @targets), 'worker exit records completed traversal once');
    @targets = map { fixture($s, "/cache/exit/$_", $prefix, $s->{cache}) } 1 .. $files;
    is(request($s, '/cache/exit*', 'PURGE')->{status}, 202, 'shutdown work accepted');
    await(sub { status($s)->{cache}{active} }, 'shutdown walk never started', 4);
    stop_server($s);
    ok(!grep({ -e $_ } @targets), 'graceful shutdown finishes the active walk');
};
}

subtest 'admission wait and disconnect do not perform request-bound I/O' => sub {
    my $s = start_server('admission', workers => 1, capacity => 1, throttle => '60s');
    my $prefix = warm($s);
    my @targets = map { fixture($s, "/cache/wait/$_", $prefix, $s->{cache}) } 1 .. 128;
    is(request($s, '/cache/pending*', 'PURGE')->{status}, 202, 'queue occupied');
    my $client = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => $s->{port}) or die $!;
    print {$client} "PURGE /cache/wait* HTTP/1.0\r\nHost: localhost\r\n\r\n";
    await(sub { status($s)->{cache}{requests}{waiting} == 1 }, 'request never waited');
    is(request($s, '/health')->{status}, 200, 'worker serves HTTP while admission waits');
    ok(!grep({ !-e $_ } @targets), 'waiting request performs no deletion');
    close $client;
    await(sub { status($s)->{cache}{requests}{waiting} == 0 }, 'disconnected waiter leaked');
    is(status($s)->{cache}{requests}{overflow}, 1, 'one waiting request counted once');
    is(status($s)->{queue}{size}, 1, 'disconnect created no second task');
    stop_server($s);

    $s = start_server('admission-release', workers => 1, capacity => 1, throttle => '100ms');
    is(request($s, '/cache/first*', 'PURGE')->{status}, 202, 'first credit accepted');
    is(request($s, '/cache/second*', 'PURGE')->{status}, 202, 'second request admitted after release');
    drain($s);
    is(status($s)->{cache}{requests}{waiting}, 0, 'no waiter remains after admission');
    stop_server($s);
};

subtest 'log-phase HIT/MISS accounting and named pool validation' => sub {
    my $s = start_server('observation', workers => 1);
    warm($s);
    await(sub { my $v = status($s)->{cache}{responses}; $v->{hit} >= 1 && $v->{miss} >= 1 },
          'cache status was not observed');
    my $before = status($s)->{cache}{responses};
    request($s, '/health');
    is_deeply(status($s)->{cache}{responses}, $before, 'uncached traffic is not a MISS');
    if ($ENV{TEST_NGINX_THREADS}) {
        my $text = config($s);
        $text =~ s/cache_purge_thread_pool cleanup;/cache_purge_thread_pool unknown_pool;/;
        isnt(check_config($s, $text), 0, 'undefined named pool rejected');
        like(read_file("$s->{root}/config.log"), qr/unknown_pool.*unknown|unknown.*unknown_pool/,
             'core pool validation diagnostic');
    }
    stop_server($s);
};

subtest 'configuration and synchronous guards' => sub {
    my $s = start_server('configuration', background => 0);
    my $initial = status($s);
    is($initial->{queue}{size}, 0, 'empty queue with background disabled');
    is($initial->{queue}{max_size}, $s->{capacity}, 'status allocates its shared metrics zone');
    is($initial->{cache}{responses}{hit}, 0, 'no fabricated cache hit');
    for my $target ("$s->{cache}/missing", $s->{temp}, "$s->{root}/unknown") {
        my $text = config($s);
        $text =~ s{cache_purge_queue_status \Q$s->{cache}\E///;}{cache_purge_queue_status $target;};
        isnt(check_config($s, $text), 0, 'invalid cache path rejected');
        like(read_file("$s->{root}/config.log"),
             qr/\[emerg\].*cache_purge_queue_status: unknown cache path/,
             'configuration diagnostic');
    }
    for my $name (qw(background_queue legacy_status vary_aware)) {
        my $directive = "cache_purge_$name";
        my $text = config($s);
        $text =~ s/^\s*\Q$directive\E\s+\w+;\n//m;
        $text =~ s/http \{/http {\n    $directive on; $directive off;/;
        is(check_config($s, $text), 0, "$directive retains last-value-wins parsing")
            or diag(read_file("$s->{root}/config.log"));
        $text =~ s/\Q$directive\E on;/$directive invalid;/;
        isnt(check_config($s, $text), 0, "$directive rejects invalid value");
        like(read_file("$s->{root}/config.log"),
             qr/"\Q$directive\E" directive has invalid value "invalid", expected "on" or "off"/,
             'flag diagnostic identifies the directive and accepted values');
    }
    for my $batch (0, 64, 65) {
        my $text = config($s);
        $text =~ s/cache_purge_batch_size \d+;/cache_purge_batch_size $batch;/;
        my $rc = check_config($s, $text);
        if (!$batch) {
            isnt($rc, 0, 'zero batch limit rejected');
            like(read_file("$s->{root}/config.log"),
                 qr/\[emerg\].*cache_purge_batch_size must be greater than 0/,
                 'zero batch limit diagnostic');
        } else {
            is($rc, 0, 'positive batch limit accepted')
                or diag(read_file("$s->{root}/config.log"));
            my $log = read_file("$s->{root}/config.log");
            if ($batch > 64) {
                like($log, qr/\[warn\].*cache_purge_batch_size 65 exceeds 64, using 64/,
                     'batch limit clamped with a warning');
            } else {
                unlike($log, qr/cache_purge_batch_size.*exceeds/,
                       'maximum batch limit needs no warning');
            }
        }
    }
    for my $protocol (qw(proxy fastcgi scgi uwsgi)) {
        for my $rule ('PURGE purge_all from', 'PURGE from all 127.0.0.1') {
            my $text = config($s);
            $text =~ s!location /health \{!
                location = /invalid { ${protocol}_cache_purge $rule; }
                location /health {!;
            is(check_config($s, $text), 1 << 8,
               "$protocol rejects a malformed access list without a signal");
            like(read_file("$s->{root}/config.log"),
                 qr/missing address after "from"|invalid parameter .* after "all"/,
                 'access-list diagnostic names the invalid argument');
        }
    }
    my $prefix = warm($s);
    my $protected = fixture($s, '/cache/sync/keep', $prefix, "$s->{temp}/nested");
    my $normal = fixture($s, '/cache/sync/delete', $prefix);
    is(request($s, '/cache/sync*', 'PURGE')->{status}, 200, 'synchronous purge');
    ok(-e $protected && !-e $normal, 'synchronous path protection');
    is(status($s)->{cache}{files}{deleted}, 0, 'synchronous work excluded from metric');
    stop_server($s);
};

subtest 'capacity, duplicate tasks and reload ownership' => sub {
    my $s = start_server('reload', capacity => 1, throttle => '60s');
    my $prefix = warm($s);
    my $protected = fixture($s, '/cache/reload/keep', $prefix, "$s->{temp}/nested");
    my $normal = fixture($s, '/cache/reload/delete', $prefix);
    is(request($s, '/cache/reload*', 'PURGE')->{status}, 202, 'task queued');
    is(request($s, '/cache/reload*', 'PURGE')->{status}, 202, 'duplicate accepted at capacity');
    is(status($s, '/status-a/child')->{cache}{pending}{total}, 1, 'inherited endpoint');
    ok(status($s, '/status-b')->{queue}{full}, 'zone capacity visible from other cache');
    is(status($s, '/status-b')->{cache}{pending}{total}, 0, 'depth remains per path');
    is(status($s)->{cache}{requests}{deduplicated}, 1, 'duplicate counted once');
    $s->{resize_rejected} = 1;
    for my $size (2, 100000) {
        my $before = length read_file("$s->{root}/error.log");
        $s->{capacity} = $size;
        write_file("$s->{root}/nginx.conf", config($s));
        kill 'HUP', $s->{pid};
        await(sub {
            substr(read_file("$s->{root}/error.log"), $before)
                =~ /cache_purge_queue_size changes (?:from 1 to $size|the shared zone size)/;
        }, 'resize was accepted');
        is(status($s)->{queue}{max_size}, 1, 'failed reload retained old capacity');
    }
    $s->{capacity} = 1;
    my @old = read_file("$s->{root}/error.log") =~ /start worker process (\d+)/g;
    is(scalar @old, $workers, 'original worker generation identified');
    $s->{stopped} = \@old;
    kill 'STOP', @old;
    $s->{temp} = "$s->{cache}/new-custom-spool";
    reload_server($s);
    is(status($s)->{cache}{pending}{total}, 1, 'task still pending in new generation');
    ok(-e $normal && -e $protected, 'old generation did not finish the walk');
    kill 'CONT', @old;
    $s->{stopped} = [];
    await(sub {
        my $log = read_file("$s->{root}/error.log");
        !grep { $log !~ /worker process $_ exited with code 0/ } @old;
    }, 'old workers did not exit');
    $s->{throttle} = '10ms';
    reload_server($s);
    drain($s);
    ok(-e $protected && !-e $normal, 'new worker retained original protection');
    is(status($s)->{cache}{requests}{deduplicated}, 1, 'duplicate total survived reload');
    is(status($s)->{cache}{files}{deleted}, 1, 'completed walk recorded once');
    stop_server($s);

    $s = start_server('all', capacity => 2, throttle => '60s');
    is(request($s, '/all', 'PURGE')->{status}, 202, 'purge-all queued');
    is(status($s)->{cache}{pending}{full}, 1, 'one full-cache task is pending');
    is(request($s, '/all/other', 'PURGE')->{status}, 202,
       'equivalent purge-all coalesced');
    is(status($s)->{cache}{pending}{total}, 1, 'one pending purge-all');
    is(request($s, '/cache/*', 'PURGE')->{status}, 202,
       'wildcard remains a distinct task');
    is(status($s)->{cache}{pending}{total}, 2, 'both operation types retained');
    $s->{background} = 0;
    $s->{throttle} = '10ms';
    reload_server($s);
    drain($s);
    is(status($s)->{cache}{pending}{full}, 0, 'full-cache count clears after drain');
    stop_server($s);

    $s = start_server('disable', throttle => '60s');
    $prefix = warm($s);
    $normal = fixture($s, '/cache/disable/delete', $prefix);
    is(request($s, '/cache/disable*', 'PURGE')->{status}, 202, 'accepted before disable');
    $s->{background} = 0;
    $s->{throttle} = '10ms';
    reload_server($s);
    drain($s);
    ok(!-e $normal, 'accepted task drained after background disable');
    stop_server($s);
};

done_testing();
