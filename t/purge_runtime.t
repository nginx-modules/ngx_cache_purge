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
require(dirname(abs_path(__FILE__)) . '/fixtures/gateway_origin.pl');

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
    my $extra_locations = $s->{extra_locations} || '';
    my $log_level = $s->{log_level} || 'notice';
    my $cache_settings = $s->{cache_settings} || '';
    my $count = $s->{workers} || $workers;
    my $pool = $s->{threads} ? 'thread_pool cleanup threads=2 max_queue=1;' : '';
    my $selection = $s->{threads} ? 'cache_purge_thread_pool cleanup;' : '';
    # cache_purge_traffic_class: classes => [names]; the class comes from the
    # X-Class header, or with class_map from the User-Agent through a map.
    my $class_conf = '';
    if ($s->{classes}) {
        my $var = $s->{class_var} || '$http_x_class';
        if ($s->{class_map}) {
            $class_conf = "map \$http_user_agent \$npp_class { default human; \"Googlebot/2.1\" bot; }\n    ";
            $var = '$npp_class';
        }
        $class_conf .= "cache_purge_traffic_class $var @{ $s->{classes} };";
    }
    my $key = $s->{strict} ? '$cache_purge_key' : '$uri';
    my $logical = $s->{logical_key} || '$uri';
    my $origin_port = $s->{origin_port} || $s->{port};
    my $profile_host = $s->{profile_host} || 'origin.test';
    my $refresh_timeout = $s->{refresh_timeout} || '10s';
    my $namespace = $s->{strict}
        ? "cache_purge_namespace objects 1m $s->{root}/purge-state;\n"
          . "cache_purge_key objects $logical;" : '';
    # Unused extra namespaces with the longest allowed name (32 bytes). They only
    # exist to make the status "namespaces" list outgrow its reply buffer.
    $namespace .= join '', map {
        sprintf "\ncache_purge_namespace %s%02d 128k %s/purge-state;", 'n' x 30, $_, $s->{root}
    } 1 .. $s->{extra_namespaces} if $s->{strict} && $s->{extra_namespaces};
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
    my ($gateway_http, $gateway_location) = ('', '');
    if (my $g = $s->{gateway}) {
        my $gateway_settings = $cache_settings;
        $gateway_settings =~ s/proxy_/${g}_/g;
        my $length_param = $g eq 'scgi' ? 'scgi_param SCGI "1";'
            : "${g}_param CONTENT_LENGTH \"0\";";
        $gateway_http = "${g}_cache_path $s->{root}/gateway-cache levels=1:2 keys_zone=gw:2m inactive=1h;\n"
            . "${g}_temp_path $s->{root}/gateway-temp;";
        $gateway_location = <<"GATEWAY";
        location /gateway/ {
            proxy_cache_purge off;
            ${g}_cache gw;
            ${g}_cache_key $key;
            ${g}_cache_valid 200 1h;
            $gateway_settings
            ${g}_param REQUEST_METHOD \$request_method;
            ${g}_param REQUEST_URI \$request_uri;
            $length_param
            ${g}_pass 127.0.0.1:$origin_port;
            ${g}_cache_purge PURGE from 127.0.0.1;
            add_header X-Cache \$upstream_cache_status always;
        }
GATEWAY
    }
    if (my $replace = $s->{probe_replace}) {
        my ($from, $to) = @$replace;
        my $changed = $refresh =~ s/\Q$from\E/$to/g;
        die "profile fixture must replace exactly one directive: $from"
            unless $changed == 1;
    }
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
error_log $s->{root}/error.log $log_level;
events { worker_connections 1024; accept_mutex off; }
http {
    access_log off;
    $selection
    $class_conf
    $namespace
    $gateway_http
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
        $gateway_location
        $extra_locations
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
            $cache_settings
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

# cache_purge_traffic_class helpers. Class names come from the X-Class request
# header (or, with class_map, from the User-Agent through a map).
sub class_status {
    my ($s, $uri) = @_;
    my $r = request($s, $uri || '/status-a');
    die "status response: $r->{status} $r->{content}" if $r->{status} != 200;
    my $v = decode_json($r->{content});
    return ($v, $v->{cache}{responses}{classes}, $r->{content});
}

sub send_class {
    my ($s, $uri, $class, $method) = @_;
    return request($s, $uri, $method || 'GET',
                   defined $class ? { 'X-Class' => $class } : {});
}

# Reload that nginx must refuse. Returns true when the old generation is still
# the one serving, after the refusal appeared in the error log.
sub reload_refused {
    my ($s, $pattern) = @_;
    my $mark = length read_file("$s->{root}/error.log");
    my $old = $s->{generation};
    $s->{generation}++;
    write_file("$s->{root}/nginx.conf", config($s));
    kill 'HUP', $s->{pid};
    await(sub { substr(read_file("$s->{root}/error.log"), $mark) =~ $pattern },
          'the refused reload was not logged', 20);
    $s->{generation} = $old;
    return (request($s, '/status-a')->{headers}{'x-generation'} // '-1') eq $old;
}

# One namespace from the status "namespaces" list.
sub namespace_view {
    my ($s, $name) = @_;
    my $v = status($s);
    ok($v->{namespaces_complete}, 'status lists every namespace');
    my ($ns) = grep { $_->{name} eq $name } @{ $v->{namespaces} };
    die "namespace $name missing from status" unless $ns;
    return $ns;
}

# The classification documented in docs/OPERATIONS.md: what a tool reading
# cache files can conclude from the status view and a file's KEY line alone.
sub classify_generation_file {
    my ($key, $view) = @_;
    return 'legacy'
        unless $key =~ /^p4:([A-Za-z0-9_-]{1,32}):(\d{10}):([0-9A-Fa-f]{16}):/;
    my ($name, $epoch, $generation) = ($1, $2, lc $3);
    return 'unknown' unless $name eq $view->{name};
    return 'stale' if $epoch ne $view->{epoch} || $generation lt $view->{baseline};
    return $view->{rules} ? 'unknown' : 'live';
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
            if ($s->{gateway}) {
                gateway_respond($client, $s->{gateway}, $s);
                close $client;
                POSIX::_exit(0);
            }
            my $request = '';
            while (my $line = <$client>) {
                $request .= $line;
                last if $line eq "\r\n";
            }
            my $head = $request =~ /^HEAD /;
            write_file("$s->{root}/head-request-$$", $request) if $head;
            my $version = read_file("$s->{root}/version");
            my $etag = -f "$s->{root}/no-etag" ? ''
                : (-f "$s->{root}/weak-etag" ? 'W/' : '') . qq{"v$version"};
            my $modified = -f "$s->{root}/last-modified"
                ? 'Wed, 21 Oct 2015 07:28:00 GMT' : '';
            my $kind = $head ? 'head' : 'get';
            write_file("$s->{root}/get-request-$$", $request) if !$head && $s->{track_gets};
            if (-f "$s->{root}/$kind-block"
                && (!-f "$s->{root}/$kind-block-version"
                    || read_file("$s->{root}/$kind-block-version") eq $version)) {
                write_file("$s->{root}/$kind-started", $request);
                my $until = time + (-f "$s->{root}/$kind-block-version" ? 15 : 5);
                sleep 0.005 until -f "$s->{root}/$kind-release" || time >= $until;
                write_file("$s->{root}/$kind-unblocked-$version", '');
            }
            my $status = -f "$s->{root}/$kind-status"
                         ? read_file("$s->{root}/$kind-status")
                         : ($head || $s->{conditional_get})
                           && (($etag && $request =~ /If-None-Match: (?:W\/)?"v\Q$version\E"/i)
                            || (!$etag && $modified
                                && $request =~ /If-Modified-Since: \Q$modified\E/i))
                           ? 304 : 200;
            if ($status) {
                my $body = "object version $version\n";
                my $vary = -f "$s->{root}/vary" ? "Vary: Accept-Encoding\r\n" : '';
                if ($s->{vary_axes}) {
                    $vary = "Vary: X-Test-Variant, Accept-Language\r\n";
                    for my $name (qw(X-Test-Variant Accept-Language)) {
                        my ($value) = $request =~ /\r\n\Q$name\E: ([^\r\n]*)/i;
                        $body .= "$name=" . ($value || '') . "\n";
                    }
                }
                my $body_block = !$head && $status == 200 && -f "$s->{root}/body-block";
                $body = ('x' x 65536) . $body if $body_block;
                my $cache_control = $status == 200 || $status == 304
                    ? 'max-age=3600' : 'no-store';
                my $location = $head && $status >= 300 && $status < 400 && $status != 304
                    ? "Location: /origin/redirect-target\r\n" : '';
                print {$client} "HTTP/1.1 $status Test\r\nConnection: close\r\n"
                    . ($etag ? "ETag: $etag\r\n" : '')
                    . ($modified ? "Last-Modified: $modified\r\n" : '')
                    . "Cache-Control: $cache_control\r\n$vary$location"
                    . 'Content-Length: ' . length($body) . "\r\n\r\n";
                if ($body_block) {
                    print {$client} substr($body, 0, 65536);
                    write_file("$s->{root}/body-started", $request);
                    my $until = time + 15;
                    sleep 0.005 until -f "$s->{root}/body-release" || time >= $until;
                    print {$client} substr($body, 65536);
                    write_file("$s->{root}/body-finished", '');
                } elsif (!$head && $status != 304) {
                    print {$client} $body;
                }
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
    my ($s, $uri, $method, $key, $headers) = @_;
    my $socket = IO::Socket::INET->new(PeerAddr => '127.0.0.1',
                     PeerPort => $s->{port}, Timeout => 5) or die $!;
    $socket->autoflush(1);
    print {$socket} "$method $uri HTTP/1.0\r\nHost: localhost\r\n"
        . (defined $key ? "X-Refresh-Key: $key\r\n" : '')
        . join('', map { "$_: $headers->{$_}\r\n" } sort keys %{$headers || {}})
        . "\r\n";
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
    if ($s->{linked_root}) {
        make_path(dirname($s->{cache}), "$s->{cache}.real");
        symlink "$s->{cache}.real", $s->{cache} or die $!;
    }
    make_path($s->{cache}, $s->{cache_b}, "$s->{root}/origin", "$s->{root}/logs");
    write_file("$s->{root}/health", 'ready');
    write_file("$s->{root}/origin/$_", "cache fixture $_") for qw(warm item);
    start_origin($s) if $s->{controlled_origin};
    # Bind the origin first. Otherwise it can reuse the just-released NGINX
    # port and answer /health while NGINX is still retrying a failed bind.
    my $socket = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => 0,
                                     Listen => 1) or die $!;
    $s->{port} = $socket->sockport;
    close $socket;
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
        my $health = request($s, '/health');
        $health->{status} == 200 && ($health->{content} || '') eq 'ready';
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
    if ($s->{empty_etag} && $ENV{TEST_NGINX_SANITIZE}) {
        # Some NGINX versions call memcpy(h->etag, NULL, 0) when an origin
        # omits ETag. Keep this upstream defect visible as a narrow TODO.
        # Match the exact source statement/line and UBSan category, never
        # suppress other core locations or any module diagnostic.
        my $source = read_file("$ENV{NGINX_SOURCE}/src/http/ngx_http_file_cache.c");
        if ($source =~ /^(.*?)(^[ \t]*ngx_memcpy\(h->etag, c->etag\.data, c->etag\.len\);)/ms) {
            my $prefix = $1;
            my $line = 1 + ($prefix =~ tr/\n/\n/);
            my $known = qr{^.*?src/http/ngx_http_file_cache\.c:\Q$line\E:\d+: runtime error: null pointer passed as argument 2, which is declared to never be null\r?\n}m;
            my $occurrences = ($report =~ s/$known//g) + ($log =~ s/$known//g);
            if ($occurrences) {
                TODO: {
                    local $TODO = 'upstream NGINX copies an absent ETag with a NULL source and zero length';
                    fail('NGINX core empty-ETag storage is free of nonnull UB');
                }
            }
        }
    }
    unlike($report . $log, qr/AddressSanitizer|runtime error:/,
           'no unexpected sanitizer report');
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
    if ($s->{scan_root_fault}) {
        @severe = grep {
            !/\[crit\].*ngx_cache_purge: .* "\Q$s->{cache}\E" failed \(\d+: Not a directory\)/
        } @severe;
    }
    if ($s->{class_rejected}) {
        @severe = grep { !/\[emerg\].*cache_purge_traffic_class needs more distinct/ } @severe;
    }
    if ($s->{listen_fault}) {
        @severe = grep { !/\[emerg\].*(?:bind\(\) to 127\.0\.0\.1:\d+ failed \(98: (?:Address already in use|Unknown error)\)|still could not bind\(\))/ } @severe;
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
    my $last;
    my $ok = eval {
        await(sub {
            $last = request($s, '/cache/warm');
            ($last->{headers}{'x-cache'} || '') eq 'HIT';
        }, 'cache did not warm');
        1;
    };
    unless ($ok) {
        my $error = $@;
        diag("cache warmup failed: $s->{root}; nginx=$s->{port}; origin="
             . ($s->{origin_port} || $s->{port}));
        diag('last response: ' . JSON::PP::encode_json($last)) if $last;
        for my $name (qw(error.log process.log)) {
            next unless -f "$s->{root}/$name";
            my @lines = split /\n/, read_file("$s->{root}/$name");
            splice @lines, 0, @lines - 20 if @lines > 20;
            diag("$name tail:\n" . join("\n", @lines));
        }
        die $error;
    }
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

# Worker pids the master has started and not yet reported as exited.
sub live_workers {
    my ($s) = @_;
    my $log = read_file("$s->{root}/error.log");
    my %live;
    $live{$1} = 1 while $log =~ /start worker process (\d+)/g;
    delete $live{$1} while $log =~ /worker process (\d+) exited/g;
    return sort { $a <=> $b } keys %live;
}

# With $settle, also wait until every worker of the previous generation has
# exited. A retiring worker keeps accepting connections until it handles the
# quit signal (accept_mutex is off), so a request sent right after the first
# new-generation reply can still be answered by the old configuration.
sub reload_server {
    my ($s, $settle) = @_;
    my %old = map { $_ => 1 } ($settle ? live_workers($s) : ());
    $s->{generation}++;
    write_file("$s->{root}/nginx.conf", config($s));
    kill 'HUP', $s->{pid};
    await(sub { (request($s, '/status-a')->{headers}{'x-generation'} || '-1')
                eq $s->{generation} }, 'new generation did not serve a response');
    await(sub { !grep { $old{$_} } live_workers($s) },
          'the previous generation did not exit') if $settle;
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

subtest 'controlled origin cannot take the NGINX listener port' => sub {
    my $socket_new = IO::Socket::INET->can('new');
    my $last_listener;
    my $s;
    {
        no warnings 'redefine';
        # Model a legal allocator that immediately reuses a released port.
        # Keep real sockets: an occupied port must fall back to another port.
        local *IO::Socket::INET::new = sub {
            my ($class, %args) = @_;
            if (exists $args{LocalPort} && $args{LocalPort} == 0) {
                my $socket;
                $socket = $socket_new->($class, %args, LocalPort => $last_listener)
                    if $last_listener;
                $socket ||= $socket_new->($class, %args);
                $last_listener = $socket->sockport if $socket;
                return $socket;
            }
            return $socket_new->($class, %args);
        };
        $s = start_server('listener-reuse', workers => 1, strict => 1,
                          controlled_origin => 1);
    }
    isnt($s->{port}, $s->{origin_port}, 'NGINX and origin have different listeners');
    warm($s);
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'HIT',
       'warmup reaches the NGINX cache rather than the origin directly');
    stop_server($s);
};

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
    my ($s, $key, $cursor, $extra_headers) = @_;
    my %headers = (%{ $extra_headers || {} }, 'X-Refresh-Key' => $key);
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
    my $view = namespace_view($s, 'objects');
    like($view->{epoch}, qr/^\d{10}\z/, 'status reports the namespace epoch');
    is($view->{baseline}, '0000000000000001', 'fresh namespace baseline');
    is($view->{serial}, '0000000000000001', 'fresh namespace serial');
    ok(!$view->{rules}, 'fresh namespace has no per-key rules');
    is(classify_generation_file($old->[1], $view), 'live',
       'a file of the live generation is certainly live');
    my $r = request($s, '/cache/warm', 'PURGE');
    is($r->{status}, 200, 'logical invalidation returns 200');
    is(decode_json($r->{content})->{code}, 'invalidated', 'response states logical completion');
    $view = namespace_view($s, 'objects');
    ok($view->{rules}, 'exact invalidation registers a per-key rule');
    is($view->{baseline}, '0000000000000001', 'exact invalidation keeps the baseline');
    is($view->{serial}, '0000000000000002', 'exact invalidation advances the serial');
    is(classify_generation_file($old->[1], $view), 'unknown',
       'with per-key rules a file at the baseline is not claimed live or stale');
    ok(-e $old->[0], 'old file still exists while the executor is delayed');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'new request cannot hit old generation');
    await(sub { (request($s, '/cache/warm')->{headers}{'x-cache'} || '') eq 'HIT' }, 'new generation did not warm');
    my @entries = grep { $_->[1] =~ m{/cache/warm$} } cached_entries($s);
    is(scalar @entries, 2, 'new generation has a different physical filename');
    is(request($s, '/cache/*', 'PURGE')->{status}, 200, 'prefix generation invalidated');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'prefix invalidation affects exact lookups');
    is(request($s, '/all', 'PURGE')->{status}, 200, 'namespace baseline invalidated');
    $view = namespace_view($s, 'objects');
    is($view->{baseline}, $view->{serial}, 'namespace invalidation raises the baseline to the serial');
    ok(!$view->{rules}, 'namespace invalidation discards the per-key rules');
    my @earlier = cached_entries($s);
    ok(scalar @earlier, 'physical files remain while cleanup is delayed');
    is_deeply([ grep { classify_generation_file($_->[1], $view) ne 'stale' } @earlier ], [],
              'after a namespace invalidation every earlier file is certainly stale');
    my $before = status($s)->{cache}{logical}{invalidations};
    is($before, 3, 'logical invalidations have a separate metric');
    reload_server($s);
    is(status($s)->{cache}{logical}{invalidations}, $before, 'reload preserves namespace and counters');
    is_deeply(namespace_view($s, 'objects'), $view, 'reload preserves the generation state');
    stop_server($s);
    $s = start_server('strict', strict => 1, throttle => '60s');
    my $restarted = namespace_view($s, 'objects');
    isnt($restarted->{epoch}, $view->{epoch}, 'full restart creates a new epoch');
    is($restarted->{baseline}, '0000000000000001', 'full restart starts from a fresh baseline');
    my @carried = cached_entries($s);
    is_deeply([ grep { classify_generation_file($_->[1], $restarted) ne 'stale' } @carried ], [],
              'every file of the previous epoch is certainly stale after a restart');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'full restart never reuses a persisted epoch');
    stop_server($s);
};

# The status reply lists every namespace unless that would not fit its fixed
# reply buffer. Then the list stops early and says so; it is never an error.
subtest 'status namespace list is bounded and reports when it is cut short' => sub {
    my $name  = sub { ('n' x 30) . sprintf('%02d', $_[0]) };
    my $names = sub { [ map { $_->{name} } @{ $_[0]->{namespaces} } ] };

    my $s = start_server('namespaces-few', strict => 1, workers => 1, extra_namespaces => 2);
    my $v = status($s);
    ok($v->{namespaces_complete}, 'a short list is complete');
    is_deeply($names->($v), [ 'objects', map { $name->($_) } 1 .. 2 ],
              'namespaces are listed in configuration order');
    stop_server($s);

    # 81 namespaces with 32-byte names need more than the 8 KiB reply buffer.
    my @all = ('objects', map { $name->($_) } 1 .. 80);
    $s = start_server('namespaces-many', strict => 1, workers => 1, extra_namespaces => 80);
    my $r = request($s, '/status-a');
    is($r->{status}, 200, 'more namespaces than fit never turn the status reply into an error');
    cmp_ok(length $r->{content}, '<', 8192, 'the reply stays inside the 8 KiB reply buffer');
    $v = status($s);   # also checks that the reply is valid JSON with all queue/cache fields
    ok(!$v->{namespaces_complete}, 'a list that does not fit says so');
    my $got = $names->($v);
    cmp_ok(scalar @$got, '<', scalar @all, 'the list was cut short');
    cmp_ok(scalar @$got, '>=', 40, 'a useful part of the list is still reported');
    is_deeply($got, [ @all[0 .. $#$got] ], 'the reported part is the configuration-order prefix');
    is_deeply([ grep { $_->{epoch} !~ /^\d{10}\z/
                       || $_->{baseline} !~ /^[0-9a-f]{16}\z/
                       || $_->{serial} !~ /^[0-9a-f]{16}\z/ } @{ $v->{namespaces} } ], [],
              'the cut never leaves a partial entry');
    stop_server($s);
};

subtest 'FastCGI SCGI and uWSGI use real native caches with generation keys' => sub {
    for my $protocol (qw(fastcgi scgi uwsgi)) {
        for my $strict (0, 1) {
            my $s = start_server("gateway-$protocol-$strict", workers => 1,
                                 gateway => $protocol, strict => $strict,
                                 controlled_origin => 1, background => 0);
            for my $key (qw(first second)) {
                my $r = request($s, "/gateway/$key");
                is($r->{status}, 200, "$protocol: real upstream responds");
                is($r->{content}, "$protocol origin content\n", "$protocol: protocol body decoded");
                await(sub { (request($s, "/gateway/$key")->{headers}{'x-cache'} || '') eq 'HIT' },
                      "$protocol: native cache did not warm");
            }
            my $r = request($s, '/gateway/first', 'PURGE');
            is($r->{status}, 200, "$protocol: exact PURGE completed");
            is($r->{headers}{'cache-purge-code'}, $strict ? 'invalidated' : 'purged',
               "$protocol: exact result matches configured behavior");
            is(request($s, '/gateway/first')->{headers}{'x-cache'}, 'MISS',
               "$protocol: exact purge makes old entry unreachable");
            is(request($s, '/gateway/second')->{headers}{'x-cache'}, 'HIT',
               "$protocol: exact purge preserves other key");
            $r = request($s, '/gateway/*', 'PURGE');
            is($r->{status}, 200, "$protocol: prefix PURGE completed");
            is($r->{headers}{'cache-purge-code'}, $strict ? 'invalidated' : 'purged',
               "$protocol: prefix result matches configured behavior");
            is(request($s, '/gateway/second')->{headers}{'x-cache'}, 'MISS',
               "$protocol: prefix applies to native protocol cache");
            stop_server($s);
        }
    }
};

subtest 'unreadable scan roots return an explicit HTTP problem' => sub {
    for my $strict (0, 1) {
        my $s = start_server("scan-fault-$strict", workers => 1, strict => $strict,
                             background => 0, scan_root_fault => 1);
        warm($s);
        # ENOTDIR works even when the CI worker is root; chmod does not.
        rename $s->{cache}, "$s->{cache}.saved" or die $!;
        write_file($s->{cache}, 'not a cache directory');
        my $r = $strict
            ? request($s, '/refresh', 'REFRESH', { 'X-Refresh-Key' => '/cache/*' })
            : request($s, '/cache/*', 'PURGE');
        unlink $s->{cache} or die $!;
        rename "$s->{cache}.saved", $s->{cache} or die $!;
        is($r->{status}, 500, 'scan failure is not reported as success');
        is($r->{headers}{'cache-purge-code'}, 'scan_failed', 'specific scan error header');
        is($r->{headers}{'content-type'}, 'application/problem+json', 'scan error uses problem JSON');
        my $body = decode_json($r->{content});
        is($body->{code}, 'scan_failed', 'body and header agree');
        is($body->{status}, 500, 'problem status agrees with HTTP');
        unlike($r->{content}, qr/\Q$s->{cache}\E/, 'response does not disclose the cache path');
        like(read_file("$s->{root}/error.log"),
             qr/ngx_cache_purge: .* "\Q$s->{cache}\E" failed \(\d+: Not a directory\)/,
             'server log identifies the failed filesystem operation');
        is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'HIT', 'failed scan preserves the entry');
        stop_server($s);
    }
};

subtest 'REFRESH conditional decisions and bounded continuation' => sub {
    my $s = start_server('refresh', strict => 1);
    warm($s);
    is(request($s, '/cache/warm', 'REFRESH')->{status}, 405,
       'legacy PURGE capability does not accept REFRESH');
    for my $method (qw(GET HEAD POST PURGE OPTIONS)) {
        my $wrong = request($s, '/refresh', $method);
        is($wrong->{status}, 405, "$method: REFRESH endpoint restricts its method");
        is($wrong->{headers}{'allow'}, 'REFRESH', "$method: Allow identifies the implemented method");
        is($wrong->{headers}{'cache-purge-code'}, 'method_not_allowed', "$method: stable error code");
        is($wrong->{headers}{'cache-control'}, 'no-store', "$method: error is not cacheable");
        if ($method eq 'HEAD') {
            # HTTP::Tiny omits content for HEAD itself; inspect the wire too.
            my (undef, $body) = split /\r\n\r\n/,
                finish_request(pending_request($s, '/refresh', 'HEAD')), 2;
            is($body // '', '', 'HEAD omits the error body on the wire');
        } else {
            is(decode_json($wrong->{content})->{status}, 405, 'problem status agrees with HTTP');
        }
    }
    is(request($s, '/probe')->{status}, 404, 'internal profile is inaccessible externally');
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

subtest 'REFRESH rejects malformed cursors before probing or invalidating' => sub {
    my $s = start_server('cursor-input', workers => 1, strict => 1,
                         controlled_origin => 1);
    warm($s);
    write_file("$s->{root}/head-block", '');
    write_file("$s->{root}/head-release", '');
    my @cases = (
        ['empty', "X-Cache-Purge-Cursor: \r\n"],
        ['short', 'X-Cache-Purge-Cursor: ' . ('0' x 31) . "\r\n"],
        ['long', 'X-Cache-Purge-Cursor: ' . ('0' x 33) . "\r\n"],
        ['nonhex', 'X-Cache-Purge-Cursor: ' . ('z' x 32) . "\r\n"],
        ['duplicate', 'X-Cache-Purge-Cursor: ' . ('0' x 32) . "\r\n"
            . 'x-cache-purge-cursor: ' . ('0' x 32) . "\r\n"],
    );
    for my $case (@cases) {
        my ($name, $headers) = @$case;
        my $socket = IO::Socket::INET->new(PeerAddr => '127.0.0.1',
                             PeerPort => $s->{port}, Timeout => 5) or die $!;
        $socket->autoflush(1);
        print {$socket} "REFRESH /refresh HTTP/1.0\r\nHost: localhost\r\n"
            . "X-Refresh-Key: /cache/warm\r\n$headers\r\n";
        my ($head, $json) = split /\r\n\r\n/, finish_request($socket), 2;
        like($head, qr{^HTTP/1\.[01] 400 }, "$name: malformed cursor returns 400");
        like($head, qr/\r\nCache-Purge-Code: invalid_cursor\r?$/mi,
             "$name: stable error header");
        like($head, qr{\r\nContent-Type: application/problem\+json\r?$}mi,
             "$name: problem response remains available on REFRESH");
        my $body = decode_json($json);
        is($body->{code}, 'invalid_cursor', "$name: body agrees with header");
        is($body->{status}, 400, "$name: problem status agrees with HTTP");
        ok(!-e "$s->{root}/head-started", "$name: no origin HEAD");
        is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'HIT',
           "$name: current generation is unchanged");
    }
    my $r = refresh($s, '/cache/warm', 'F' x 32);
    ok($r->{complete}, 'uppercase hexadecimal cursor is accepted');
    is_deeply($r->{results}, [], 'end cursor does not restart the scan');
    is(status($s)->{cache}{logical}{invalidations}, 0, 'cursor errors never invalidate');
    stop_server($s);
};

subtest 'REFRESH uses stored validators and does not rewrite metadata on 304' => sub {
    for my $kind (qw(weak date both none)) {
        my $s = start_server("validators-$kind", workers => 1, strict => 1,
                             controlled_origin => 1,
                             empty_etag => $kind eq 'date' || $kind eq 'none');
        write_file("$s->{root}/weak-etag", '') if $kind eq 'weak';
        write_file("$s->{root}/no-etag", '') if $kind eq 'date' || $kind eq 'none';
        write_file("$s->{root}/last-modified", '') if $kind eq 'date' || $kind eq 'both';
        warm($s);
        my ($entry) = cached_entries($s);
        write_file("$s->{root}/head-block", '');
        write_file("$s->{root}/head-release", '');
        my $r = refresh($s, '/cache/warm');
        my $head = read_file("$s->{root}/head-started");
        if ($kind eq 'weak') {
            like($head, qr/\r\nIf-None-Match: W\/"v1"\r\n/i, 'weak entity tag is preserved');
        } elsif ($kind eq 'date' || $kind eq 'both') {
            like($head, qr/\r\nIf-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT\r\n/i,
                 'cached Last-Modified becomes a valid HTTP-date condition');
            like($head, qr/\r\nIf-None-Match: "v1"\r\n/i, 'both cached validators are sent')
                if $kind eq 'both';
        }
        unlike($head, qr/\r\nIf-None-Match:/i, 'missing entity tag is omitted')
            if $kind eq 'date' || $kind eq 'none';
        if ($kind eq 'none') {
            unlike($head, qr/\r\nIf-Modified-Since:/i, 'missing modification time is omitted');
            is($r->{results}[0]{result}, 'invalidated', 'unconditional 200 invalidates conservatively');
            is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'no-validator outcome is visible');
        } else {
            is($r->{results}[0]{result}, 'kept', 'matching validator keeps current generation');
            is(read_file($entry->[0]), $entry->[2], '304 preserves file metadata, TTL and content');
            is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'HIT', '304 retains a usable entry');
        }
        stop_server($s);
    }
};

subtest 'one namespace invalidates logical keys across cache paths' => sub {
    my $s = start_server('shared-namespace', workers => 1, strict => 1,
                         throttle => '60s', extra_locations => q{
        location = /cache-b/warm {
            cache_purge_key objects /cache/warm;
            proxy_cache b;
            proxy_cache_key $cache_purge_key;
            proxy_cache_valid 200 1h;
            proxy_pass http://127.0.0.1:$server_port/origin/warm;
            add_header X-Cache $upstream_cache_status always;
        }
    });
    warm($s);
    await(sub { (request($s, '/cache-b/warm')->{headers}{'x-cache'} || '') eq 'HIT' },
          'second path did not warm');
    my @old_b;
    find(sub { push @old_b, $File::Find::name if -f $_ && /^[0-9a-f]{32}$/ },
         abs_path($s->{cache_b}));
    is(scalar @old_b, 1, 'second path has one native cache file');
    is(request($s, '/cache/warm', 'PURGE')->{status}, 200, 'first path publishes invalidation');
    is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'MISS', 'first path uses new generation');
    is(request($s, '/cache-b/warm')->{headers}{'x-cache'}, 'MISS', 'second path shares new generation');
    ok(@old_b == 1 && -e $old_b[0], 'logical invalidation does not require unlink in second path');
    is(status($s, '/status-b')->{cache}{pending}{total}, 0,
       'PURGE only schedules physical cleanup for its addressed path');
    await(sub { (request($s, '/cache-b/warm')->{headers}{'x-cache'} || '') eq 'HIT' },
          'second path did not refill');
    stop_server($s);
};

# Expire real entries without a wall-clock sleep. The offset/width comes from
# this NGINX build; the next HTTP request still takes the real core stale path.
sub expire_entries {
    my ($s) = @_;
    my ($offset, $size) = @{cache_header_layout()->{fields}{valid_sec}};
    my $template = {4 => 'L', 8 => 'Q'}->{$size} or die 'unsupported time_t';
    my $root = $s->{gateway} ? "$s->{root}/gateway-cache" : $s->{cache};
    my $count = 0;
    find(sub {
        return unless -f $_ && /^[0-9a-f]{32}$/;
        open my $fh, '+<', $_ or die $!;
        binmode $fh;
        seek($fh, $offset, 0) or die $!;
        print {$fh} pack($template, 1) or die $!;
        close $fh or die $!;
        $count++;
    }, $root);
    die 'no native entry to expire' unless $count;
}

for my $strict (0, 1) {
subtest "66 Vary representations, exact identity and reload, generation=$strict" => sub {
    my $s = start_server("vary-many-$strict", strict => $strict,
                         controlled_origin => 1, vary_axes => 1);
    my @headers = map {
        my $variant = $_;
        map { { 'X-Test-Variant' => "$variant", 'Accept-Language' => $_ } } qw(en fr);
    } 1 .. 33;
    for my $neighbor ('/cache/VARIANTS', '/cache/variantsExtra') {
        await(sub { (request($s, $neighbor)->{headers}{'x-cache'} || '') eq 'HIT' },
              'neighbor did not warm');
    }
    for my $round (1, 2) {
        for my $h (@headers) {
            await(sub {
                (request($s, '/cache/variants', 'GET', $h)->{headers}{'x-cache'} || '') eq 'HIT';
            }, 'Vary representation did not warm');
        }
        my @entries = grep { $_->[1] =~ m{(?:^|:)/cache/variants$} } cached_entries($s);
        is(scalar @entries, 66, 'all representations exist as native files');
        # Settle: with accept_mutex off a retiring worker still accepts
        # connections and closes them unanswered, which fails the PURGE below.
        reload_server($s, 1) if $round == 2;
        write_file("$s->{root}/version", $round + 1);
        is(request($s, '/cache/variants', 'PURGE')->{status}, 200, 'exact PURGE succeeds');
        for my $h (@headers) {
            my $r = request($s, '/cache/variants', 'GET', $h);
            is($r->{headers}{'x-cache'}, 'MISS', 'representation cannot reuse an old entry');
            is($r->{content}, 'object version ' . ($round + 1) . "\n"
               . "X-Test-Variant=$h->{'X-Test-Variant'}\nAccept-Language=$h->{'Accept-Language'}\n",
               'origin body identifies the requested representation');
        }
        drain($s);
        for my $neighbor ('/cache/VARIANTS', '/cache/variantsExtra') {
            is(request($s, $neighbor)->{headers}{'x-cache'}, 'HIT',
               'case and key-boundary neighbors survive');
        }
        for my $h (@headers) {
            await(sub {
                (request($s, '/cache/variants', 'GET', $h)->{headers}{'x-cache'} || '') eq 'HIT';
            }, 'replacement did not survive cleanup');
        }
    }
    stop_server($s);
};
}

subtest 'a Vary body can finish after PURGE without publishing into the new generation' => sub {
    my $s = start_server('vary-body-race', strict => 1, controlled_origin => 1,
                         vary_axes => 1);
    my $headers = { 'X-Test-Variant' => 'late', 'Accept-Language' => 'en' };
    write_file("$s->{root}/body-block", '');
    my $socket = pending_request($s, '/cache/late-vary', 'GET', undef, $headers);
    await(sub { -f "$s->{root}/body-started" }, 'origin did not start its body');
    my $head = '';
    while (my $line = <$socket>) { $head .= $line; last if $line eq "\r\n"; }
    like($head, qr/^HTTP\/1\.[01] 200 /, 'headers passed through NGINX before PURGE');
    ok(!-f "$s->{root}/body-finished", 'origin still holds the unfinished body');
    is(request($s, '/cache/late-vary', 'PURGE')->{status}, 200, 'invalidate an unfinished body');
    drain($s);
    ok(!-f "$s->{root}/body-finished", 'PURGE completes before the held body');
    write_file("$s->{root}/version", '2');
    unlink "$s->{root}/body-block" or die $!;
    write_file("$s->{root}/body-release", '');
    like(finish_request($socket), qr/object version 1\n/, 'in-flight response may finish');
    await(sub { scalar grep { $_->[1] =~ m{/cache/late-vary$} } cached_entries($s) },
          'old Vary file was not published after cleanup');
    my $next = request($s, '/cache/late-vary', 'GET', $headers);
    is($next->{headers}{'x-cache'}, 'MISS', 'late variant is unreachable');
    like($next->{content}, qr/object version 2\n/, 'new generation uses current origin');
    stop_server($s);
};

for my $protocol (qw(proxy fastcgi scgi uwsgi)) {
    my %gateway = $protocol eq 'proxy' ? () : (gateway => $protocol);
    my $uri = $protocol eq 'proxy' ? '/cache/policy' : '/gateway/policy';
    for my $policy ('lock', 'timeout', 'age') {
    subtest "$protocol cache_lock $policy cannot hold or repopulate a new generation" => sub {
        my $timeout = $policy eq 'timeout' ? '50ms' : '3s';
        my $age = $policy eq 'age' ? '50ms' : '10s';
        my $s = start_server("lock-$protocol-$policy", strict => 1,
            controlled_origin => 1, track_gets => 1, %gateway,
            cache_settings => "proxy_cache_lock on; proxy_cache_lock_timeout $timeout; "
                              . "proxy_cache_lock_age $age;");
        write_file("$s->{root}/get-block", '');
        write_file("$s->{root}/get-block-version", '1');
        my @pending = (pending_request($s, $uri, 'GET'));
        await(sub { -f "$s->{root}/get-started" }, 'lock owner did not reach origin');
        if ($policy ne 'lock') {
            push @pending, pending_request($s, $uri, 'GET');
            await(sub { my @requests = glob "$s->{root}/get-request-*"; @requests == 2 },
                  'second old-generation request did not pass the lock deadline', 3);
        }
        is(request($s, $uri, 'PURGE')->{status}, 200, 'PURGE acknowledges while owner is blocked');
        write_file("$s->{root}/version", '2');
        my $replacement = request($s, $uri);
        is($replacement->{headers}{'x-cache'}, 'MISS', 'new key obtains an independent cache lock');
        like($replacement->{content}, qr/object version 2\n/, 'new key fetches current data');
        ok(!-f "$s->{root}/get-unblocked-1", 'old lock owners are still held at origin');
        write_file("$s->{root}/get-release", '');
        like(finish_request($_), qr/object version 1\n/, 'old request can finish') for @pending;
        await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' },
              'replacement did not warm');
        like(request($s, $uri)->{content}, qr/object version 2\n/, 'late writers cannot replace new data');
        stop_server($s);
    };
    }
    subtest "$protocol cache_use_stale cannot cross an invalidated generation" => sub {
        my $s = start_server("stale-$protocol", strict => 1, controlled_origin => 1,
            track_gets => 1, %gateway,
            cache_settings => 'proxy_cache_use_stale error timeout http_503;');
        await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' }, 'stale fixture did not warm');
        expire_entries($s);
        write_file("$s->{root}/get-status", '503');
        my $stale = request($s, $uri);
        is($stale->{headers}{'x-cache'}, 'STALE', 'precondition: core stale fallback is exercised');
        like($stale->{content}, qr/object version 1\n/, 'stale body exists before PURGE');
        is(request($s, $uri, 'PURGE')->{status}, 200, 'invalidate stale generation');
        my $after = request($s, $uri);
        is($after->{status}, 503, 'origin failure remains visible after PURGE');
        isnt($after->{headers}{'x-cache'} || '', 'STALE', 'old generation is not a stale fallback');
        unlink "$s->{root}/get-status" or die $!;
        write_file("$s->{root}/version", '2');
        like(request($s, $uri)->{content}, qr/object version 2\n/, 'origin recovery fills current data');
        stop_server($s);
    };
    subtest "$protocol cache_min_uses starts again for a new generation" => sub {
        my $s = start_server("min-uses-$protocol", strict => 1, controlled_origin => 1,
            track_gets => 1, %gateway, cache_settings => 'proxy_cache_min_uses 3;');
        for my $version (1, 2) {
            write_file("$s->{root}/version", $version);
            for (1 .. 2) {
                my $r = request($s, $uri);
                is($r->{headers}{'x-cache'}, 'MISS', 'pre-threshold request reaches origin');
                like($r->{content}, qr/object version $version\n/, 'current origin body is used');
            }
            await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' },
                  'minimum-use threshold did not allow a cache fill');
            if ($version == 1) {
                is(request($s, $uri, 'PURGE')->{status}, 200, 'invalidate the warmed generation');
            }
        }
        stop_server($s);
    };
}

for my $protocol (qw(proxy fastcgi scgi uwsgi)) {
subtest "$protocol bypass and no_cache retain distinct read/write policies across PURGE" => sub {
    my %gateway = $protocol eq 'proxy' ? () : (gateway => $protocol);
    my $s = start_server("cache-predicates-$protocol", strict => 1, controlled_origin => 1,
        track_gets => 1, %gateway,
        cache_settings => 'proxy_cache_bypass $http_x_test_bypass; proxy_no_cache $http_x_test_no_store;');
    my $uri = $protocol eq 'proxy' ? '/cache/predicates' : '/gateway/predicates';
    await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' }, 'predicate fixture did not warm');
    write_file("$s->{root}/version", '2');
    is(request($s, $uri, 'GET', { 'X-Test-No-Store' => 1 })->{headers}{'x-cache'}, 'HIT',
       'no_cache alone does not bypass a stored response');
    my $bypass = request($s, $uri, 'GET', { 'X-Test-Bypass' => 1, 'X-Test-No-Store' => 1 });
    is($bypass->{headers}{'x-cache'}, 'BYPASS', 'read bypass is exercised');
    like($bypass->{content}, qr/object version 2\n/, 'bypass fetches from origin');
    like(request($s, $uri)->{content}, qr/object version 1\n/, 'no_cache prevents replacement');
    is(request($s, $uri, 'PURGE')->{status}, 200, 'PURGE invalidates the stored generation');
    for (1 .. 2) {
        is(request($s, $uri, 'GET', { 'X-Test-No-Store' => 1 })->{headers}{'x-cache'}, 'MISS',
           'no_cache prevents storing a new generation');
    }
    await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' }, 'cache did not recover');
    like(request($s, $uri)->{content}, qr/object version 2\n/, 'ordinary requests cache current data');
    stop_server($s);
};
}

subtest 'late core revalidation cannot refresh a replacement generation' => sub {
    my $s = start_server('core-revalidation', strict => 1, controlled_origin => 1,
        conditional_get => 1, cache_settings => 'proxy_cache_revalidate on;');
    my $uri = '/cache/revalidated';
    await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' }, 'revalidation fixture did not warm');
    expire_entries($s);
    write_file("$s->{root}/get-block", '');
    write_file("$s->{root}/get-block-version", '1');
    my $old = pending_request($s, $uri, 'GET');
    await(sub { -f "$s->{root}/get-started" }, 'conditional GET did not start');
    like(read_file("$s->{root}/get-started"), qr/If-None-Match: "v1"/i, 'core supplies cached validator');
    is(request($s, $uri, 'PURGE')->{status}, 200, 'invalidate during revalidation');
    write_file("$s->{root}/version", '2');
    like(request($s, $uri)->{content}, qr/object version 2\n/, 'replacement uses current origin');
    ok(!-f "$s->{root}/get-unblocked-1", 'old conditional response is still held');
    write_file("$s->{root}/get-release", '');
    my $response = finish_request($old);
    like($response, qr/X-Cache: REVALIDATED/i, 'old request completes real core 304 path');
    await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' }, 'replacement did not warm');
    like(request($s, $uri)->{content}, qr/object version 2\n/, '304 did not change replacement identity');
    stop_server($s);
};

subtest 'background update cannot publish into a replacement generation' => sub {
    my $source = read_file("$ENV{NGINX_SOURCE}/src/http/modules/ngx_http_proxy_module.c");
    plan skip_all => 'core predates cache_background_update'
        unless $source =~ /"proxy_cache_background_update"/;
    my $s = start_server('core-background', strict => 1, controlled_origin => 1,
        cache_settings => 'proxy_cache_use_stale updating; proxy_cache_background_update on;');
    my $uri = '/cache/background';
    await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' }, 'background fixture did not warm');
    my ($old) = grep { $_->[1] =~ m{/cache/background$} } cached_entries($s);
    expire_entries($s);
    write_file("$s->{root}/version", '2');
    write_file("$s->{root}/get-block", '');
    write_file("$s->{root}/get-block-version", '2');
    is(request($s, $uri)->{headers}{'x-cache'}, 'STALE', 'first expired request starts background update');
    await(sub { -f "$s->{root}/get-started" }, 'background origin request did not start');
    is(request($s, $uri)->{headers}{'x-cache'}, 'UPDATING', 'concurrent old request serves stale while updating');
    is(request($s, $uri, 'PURGE')->{status}, 200, 'invalidate during background update');
    write_file("$s->{root}/version", '3');
    my $next = request($s, $uri);
    is($next->{headers}{'x-cache'}, 'MISS', 'new generation does not use updating entry');
    like($next->{content}, qr/object version 3\n/, 'replacement has current body');
    drain($s);
    ok(!-f "$s->{root}/get-unblocked-2", 'old background response is held through cleanup');
    write_file("$s->{root}/get-release", '');
    await(sub { -f $old->[0] && read_file($old->[0]) =~ /object version 2\n/ },
          'late background cache write did not complete');
    await(sub { (request($s, $uri)->{headers}{'x-cache'} || '') eq 'HIT' }, 'replacement did not warm');
    like(request($s, $uri)->{content}, qr/object version 3\n/, 'late background body stays unreachable');
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
    await(sub { (request($s, '/cache/late')->{headers}{'x-cache'} || '') eq 'HIT' },
          'concurrent replacement did not become a HIT');
    write_file("$s->{root}/head-release", '');
    my (undef, $body) = split /\r\n\r\n/, finish_request($probe), 2;
    is(decode_json($body)->{results}[0]{result}, 'raced_replaced', 'old HEAD cannot invalidate a replacement');
    is(request($s, '/cache/late')->{headers}{'x-cache'}, 'HIT', 'replacement is still reachable');
    stop_server($s);
};

subtest 'REFRESH failure policy, URI identity and deadlines' => sub {
    my $s = start_server('probe-errors', strict => 1, controlled_origin => 1,
                         logical_key => '$request_uri');
    my $key = '/cache/query?part=a%2Fb&other=1';
    await(sub { (request($s, $key)->{headers}{'x-cache'} || '') eq 'HIT' }, 'query key did not warm');
    write_file("$s->{root}/head-block", '');
    write_file("$s->{root}/head-release", '');
    my $r = refresh($s, $key, undef, {
        'Authorization' => 'Bearer client-secret', 'Cookie' => 'session=client-secret',
        'If-None-Match' => '"client-tag"', 'If-Modified-Since' => 'client-date',
        'Accept-Language' => 'client-language', 'Range' => 'bytes=0-1',
    });
    is($r->{results}[0]{result}, 'kept', 'query representation receives a conditional 304');
    my $head = read_file("$s->{root}/head-started");
    like($head, qr{^HEAD /origin/query\?part=a%2Fb&other=1 HTTP/}, 'escaped URI and query survive the explicit mapping');
    like($head, qr/\r\nHost: origin\.test\r\n/i, 'literal profile Host is used');
    unlike($head, qr/\r\n(?:Authorization|Cookie):/i, 'administrative credentials are not forwarded');
    unlike($head, qr/client-secret|client-tag|client-date|client-language|\r\nRange:/i,
           'client headers cannot replace cached validators or select another representation');
    like($head, qr/\r\nIf-None-Match: "v1"\r\n/i, 'probe uses the cached entity tag');
    for my $status (301, 302, 303, 307, 308, 401, 403, 412, 429, 500, 503, 0, 410) {
        write_file("$s->{root}/head-status", "$status");
        my @before = glob "$s->{root}/head-request-*";
        $r = refresh($s, $key);
        my @after = glob "$s->{root}/head-request-*";
        is(scalar @after, 1 + @before, 'redirect Location does not trigger another origin request')
            if $status >= 300 && $status < 400;
        is($r->{results}[0]{upstream_status}, $status, 'raw origin status remains distinct from local errors');
        is($r->{results}[0]{result}, $status == 410 ? 'invalidated'
                                   : $status ? 'upstream_status' : 'transport_error',
           'explicit result for origin or transport failure');
        is(request($s, $key)->{headers}{'x-cache'}, 'HIT',
           'redirect or origin failure preserves the cached entry') unless $status == 410;
    }
    # The 410 decision invalidated the entry. A completed GET is not a cache
    # publication barrier: wait for an actual HIT before testing another probe.
    await(sub { (request($s, $key)->{headers}{'x-cache'} || '') eq 'HIT' },
          'entry did not refill after 410');
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

subtest 'REFRESH profile validation waits for a real cache candidate' => sub {
    my $s = start_server('profile-refill', workers => 1, strict => 1,
                         controlled_origin => 1, profile_host => '$http_host');
    my $key = '/cache/warm';
    warm($s);
    is(request($s, $key, 'PURGE')->{status}, 200, 'old generation invalidated');
    write_file("$s->{root}/get-block", '');
    my $get = pending_request($s, $key, 'GET');
    await(sub { -f "$s->{root}/get-started" }, 'refill did not reach origin');
    write_file("$s->{root}/head-block", '');
    write_file("$s->{root}/head-release", '');
    my $r = refresh($s, $key);
    ok($r->{scan_complete} && $r->{complete}, 'empty current generation is a complete scan');
    is_deeply($r->{results}, [], 'in-flight refill is not yet a REFRESH candidate');
    ok(!-e "$s->{root}/head-started", 'no HEAD without a current candidate');

    write_file("$s->{root}/get-release", '');
    like(finish_request($get), qr/object version 1/, 'refill response completed');
    await(sub { (request($s, $key)->{headers}{'x-cache'} || '') eq 'HIT' },
          'refill did not become a cache HIT');
    $r = refresh($s, $key);
    is(scalar @{$r->{results}}, 1, 'profile check has one current cache candidate');
    is($r->{results}[0]{result}, 'skipped_profile', 'client-controlled Host profile is rejected');
    like(read_file("$s->{root}/error.log"),
         qr/\[error\].*ngx_cache_purge: REFRESH profile "[^"]+" rejected: header "Host" must have a literal value/,
         'REFRESH diagnostic identifies the profile and rejected header');
    ok(!-e "$s->{root}/head-started", 'rejected Host never reaches the origin');
    is(request($s, $key)->{headers}{'x-cache'}, 'HIT', 'rejected profile preserves the cached entry');
    stop_server($s);
};

subtest 'disconnect cancels a REFRESH page and releases its worker' => sub {
    my $s = start_server('refresh-disconnect', workers => 1, strict => 1,
                         controlled_origin => 1, log_level => 'info');
    for my $key (qw(first second third)) {
        await(sub { (request($s, "/cache/$key")->{headers}{'x-cache'} || '') eq 'HIT' },
              'disconnect fixture did not warm');
    }
    write_file("$s->{root}/head-block", '');
    my $client = pending_request($s, '/refresh', 'REFRESH', '/cache/*');
    await(sub { -e "$s->{root}/head-started" }, 'REFRESH did not start its first probe');
    close $client;
    # Observe cancellation while the origin is still blocked, then let its
    # socket finish before a new page claims the scanner.
    await(sub {
        read_file("$s->{root}/error.log") =~ /client prematurely closed connection/;
    }, 'REFRESH client disconnect was not observed');
    write_file("$s->{root}/head-release", '');
    my $r = refresh($s, '/cache/first');
    ok($r->{complete}, 'next page can claim the worker after disconnect');
    is($r->{results}[0]{result}, 'kept', 'next page can obtain and release a probe credit');
    my @heads = glob "$s->{root}/head-request-*";
    is(scalar @heads, 2, 'cancelled page sent only its already-started HEAD');
    is(status($s)->{cache}{logical}{invalidations}, 0, 'disconnect causes no invalidation');
    for my $key (qw(first second third)) {
        is(request($s, "/cache/$key")->{headers}{'x-cache'}, 'HIT', 'entry survives cancelled page');
    }
    stop_server($s);
};

subtest 'REFRESH unsafe profiles cannot send origin probes' => sub {
    my @cases = (
        ['public', 'internal;', '# no internal restriction', 'location must be internal'],
        ['headers', 'proxy_pass_request_headers off;', 'proxy_pass_request_headers on;',
         '"proxy_pass_request_headers" must be off'],
        ['body', 'proxy_pass_request_body off;', 'proxy_pass_request_body on;',
         '"proxy_pass_request_body" must be off'],
        ['intercept', 'proxy_cache off;', 'proxy_cache off; proxy_intercept_errors on;',
         '"proxy_intercept_errors" must be off'],
        ['cache', 'proxy_cache off;', 'proxy_cache a;', '"proxy_cache" must be off'],
        ['method', 'proxy_method HEAD;', 'proxy_method GET;', '"proxy_method" must be HEAD'],
        ['host', 'proxy_set_header Host origin.test;', 'proxy_set_header Host "";',
         'a non-empty literal "Host" header is required'],
        ['etag', 'proxy_set_header If-None-Match $cache_purge_refresh_etag;',
         'proxy_set_header If-None-Match "";', '"If-None-Match" must be $cache_purge_refresh_etag'],
        ['modified', 'proxy_set_header If-Modified-Since $cache_purge_refresh_last_modified;',
         'proxy_set_header If-Modified-Since "";',
         '"If-Modified-Since" must be $cache_purge_refresh_last_modified'],
        ['representation', 'proxy_set_header Host origin.test;',
         'proxy_set_header Host origin.test; proxy_set_header Accept-Language $http_accept_language;',
         'header "Accept-Language" must have a literal value'],
        ['redirect', 'internal;', 'internal; try_files /missing-probe-fixture /probe-redirect;',
         'internal redirect changed the profile URI'],
    );
    for my $case (@cases) {
        my ($name, $from, $to, $reason) = @$case;
        my $s = start_server("profile-$name", workers => 1, strict => 1,
                             controlled_origin => 1, probe_replace => [$from, $to],
                             extra_locations => 'location = /probe-redirect { internal; }');
        warm($s);
        write_file("$s->{root}/head-block", '');
        write_file("$s->{root}/head-release", '');
        my $r = refresh($s, '/cache/warm');
        is(scalar @{$r->{results}}, 1, "$name: a current candidate was checked");
        is($r->{results}[0]{result}, 'skipped_profile', "$name: unsafe profile rejected");
        ok(!-e "$s->{root}/head-started", "$name: no origin HEAD was sent");
        is(request($s, '/cache/warm')->{headers}{'x-cache'}, 'HIT', "$name: entry remains usable");
        like(read_file("$s->{root}/error.log"), qr/\[error\].*ngx_cache_purge: REFRESH profile .*\Q$reason\E/,
             "$name: actionable diagnostic");
        stop_server($s);
    }
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

subtest 'traffic class HIT/MISS counters are subsets of the totals' => sub {
    # Default worker count: the class counters of every process slot are summed.
    my $s = start_server('classes', classes => [qw(bot human)]);
    warm($s);
    my $totals = sub { my ($v) = class_status($s); $v->{cache}{responses} };
    await(sub { my $t = $totals->(); $t->{hit} >= 1 && $t->{miss} >= 1 },
          'warm-up was not observed');
    my (undef, undef, $raw) = class_status($s);
    like($raw, qr/"hit_ratio":\d\.\d{6},"classes":\{"bot":\{"hit":0,"miss":0,"hit_ratio":0\.000000\},"human":\{"hit":0,"miss":0,"hit_ratio":0\.000000\}\}\}\},"namespaces":\[/,
         'classes sit inside responses, in configuration order, all zero before any class traffic');

    write_file("$s->{root}/origin/fresh", 'fresh fixture');
    my $before = $totals->();
    my (%seen, $counted);
    my $send = sub {
        my ($n, $uri, $class) = @_;
        for (1 .. $n) {
            my $x = send_class($s, $uri, $class)->{headers}{'x-cache'} || '';
            $seen{ defined $class ? $class : '-' }{$x}++;
            $seen{'*'}{$x}++;
            $counted++ if $x eq 'HIT' || $x eq 'MISS';
        }
    };
    $send->(12, '/cache/warm', 'bot');
    $send->(7,  '/cache/warm', 'human');
    $send->(5,  '/cache/warm', 'other');   # a value that names no class: totals only
    $send->(3,  '/cache/warm', undef);     # variable not found: totals only
    $send->(2,  '/cache/warm', 'bots');    # a prefix of a name is not the name
    $send->(2,  '/cache/warm', 'Bot');     # a different letter case is a different name
    $send->(2,  '/cache/fresh', 'human');  # a MISS, then (normally) a HIT
    # Never counted, whatever the header says: no cache is involved.
    send_class($s, '/health', 'bot');
    send_class($s, '/cache/none*', 'bot', 'PURGE');
    send_class($s, '/status-a', 'bot');

    await(sub {
        my $t = $totals->();
        ($t->{hit} + $t->{miss}) - ($before->{hit} + $before->{miss}) == $counted;
    }, 'counted requests were not observed');
    my ($v, $c) = class_status($s);
    for my $name (qw(bot human)) {
        is($c->{$name}{hit},  $seen{$name}{HIT}  || 0, "$name HIT count");
        is($c->{$name}{miss}, $seen{$name}{MISS} || 0, "$name MISS count");
        my $n = $c->{$name}{hit} + $c->{$name}{miss};
        cmp_ok(abs($c->{$name}{hit_ratio} - ($n ? $c->{$name}{hit} / $n : 0)), '<', 1e-6,
               "$name hit_ratio");
    }
    cmp_ok($seen{human}{MISS} || 0, '>=', 1, 'the fresh URI produced a class MISS');
    is($v->{cache}{responses}{hit} - $before->{hit}, $seen{'*'}{HIT} || 0,
       'totals carry every HIT, classified or not');
    cmp_ok($c->{bot}{hit} + $c->{human}{hit}, '<', $v->{cache}{responses}{hit} - $before->{hit},
           'classes are a strict subset of the totals');
    stop_server($s);
};

subtest 'traffic class variable can be a map result' => sub {
    my $s = start_server('classes-map', workers => 1, classes => [qw(bot human)],
                         class_map => 1);
    warm($s);
    my $count = sub { my ($v, $c) = class_status($s); [ $c->{bot}{hit}, $c->{human}{hit} ] };
    await(sub { my $t = (class_status($s))[0]->{cache}{responses}; $t->{hit} >= 1 },
          'warm-up was not observed');
    my $b = $count->();
    request($s, '/cache/warm', 'GET', { 'User-Agent' => 'Googlebot/2.1' }) for 1 .. 4;
    request($s, '/cache/warm', 'GET', { 'User-Agent' => 'Mozilla/5.0' }) for 1 .. 3;
    await(sub { my $a = $count->(); $a->[0] - $b->[0] == 4 && $a->[1] - $b->[1] >= 3 },
          'map-derived classes were not counted');
    my $a = $count->();
    is($a->[0] - $b->[0], 4, 'bot user agents counted under bot');
    is($a->[1] - $b->[1], 3, 'other user agents counted under human');
    stop_server($s);
};

subtest 'without the directive the status reply is unchanged' => sub {
    my $s = start_server('classes-off', workers => 1);
    my $r = request($s, '/status-a');
    unlike($r->{content}, qr/classes/, 'no classes member');
    like($r->{content}, qr/"responses":\{"hit":\d+,"miss":\d+,"hit_ratio":\d\.\d{6}\}\},"namespaces":\[/,
         'responses closes right after hit_ratio');
    ok(!exists decode_json($r->{content})->{cache}{responses}{classes}, 'no classes key');
    status($s);   # the documented field set is still complete
    stop_server($s);
};

subtest 'class counters follow names across reload, reorder, drop and re-add' => sub {
    my $s = start_server('classes-reload', workers => 1, classes => [qw(bot human)]);
    warm($s);
    my $hits = sub {
        my (undef, $c) = class_status($s);
        return { map { $_ => $c->{$_}{hit} } keys %$c };
    };
    my $send = sub { my ($class, $n) = @_; send_class($s, '/cache/warm', $class) for 1 .. $n };

    $send->('bot', 3);
    $send->('human', 2);
    await(sub { my $h = $hits->(); ($h->{bot} // -1) == 3 && ($h->{human} // -1) == 2 },
          'initial class counts were not observed');

    $s->{classes} = [qw(human bot)];
    reload_server($s, 1);
    is_deeply($hits->(), { bot => 3, human => 2 }, 'reorder keeps every count under its name');
    my (undef, undef, $raw) = class_status($s);
    like($raw, qr/"classes":\{"human":.*"bot":/, 'the reply follows the new order');

    $s->{classes} = [qw(human)];
    reload_server($s, 1);
    is_deeply($hits->(), { human => 2 }, 'a dropped name leaves the reply');
    $send->('human', 4);
    $send->('bot', 5);   # not a class any more: totals only
    await(sub { ($hits->()->{human} // -1) == 6 }, 'human traffic after the drop');

    $s->{classes} = [qw(bot human)];
    reload_server($s, 1);
    is_deeply($hits->(), { bot => 3, human => 6 },
              're-added name resumes its old count; traffic sent while dropped is not attributed');
    stop_server($s);
};

subtest 'a refused class reload keeps the old configuration and consumes no slot' => sub {
    my $s = start_server('classes-slots', workers => 1, classes => [qw(a b)]);
    $s->{class_rejected} = 1;
    my $refused = qr/\[emerg\].*cache_purge_traffic_class needs more distinct class names/;
    my $names = sub { my (undef, $c) = class_status($s); [ sort keys %$c ] };

    # Two new names, one free slot: refused as a whole.
    $s->{classes} = [qw(c d)];
    is(check_config($s, config($s)), 0, 'nginx -t cannot see the shared-zone limit')
        or diag(read_file("$s->{root}/config.log"));
    ok(reload_refused($s, $refused), 'two new names with one free slot: old generation keeps serving');
    is_deeply($names->($s), [qw(a b)], 'the live reply still reports the old classes');

    # The refused names did not take the free slot.
    $s->{classes} = [qw(a b c)];
    reload_server($s, 1);
    $s->{classes} = [qw(c b a)];
    reload_server($s, 1);
    is_deeply($names->($s), [qw(a b c)], 'all three slots are usable after a refusal');

    # Zone full: a fourth distinct name is refused, known names still work.
    $s->{classes} = [qw(a b d)];
    ok(reload_refused($s, $refused), 'a fourth distinct name is refused');
    $s->{classes} = [qw(c a b)];
    reload_server($s, 1);
    is_deeply($names->($s), [qw(a b c)], 'the refusal changed nothing');
    stop_server($s);
};

# A reload that fails after the zone is attached (here: a listener that cannot
# be bound) must leave every class slot free; see traffic_class_bind in queue.c.
subtest 'a reload that fails after the shared zone is attached keeps its slots free' => sub {
    my $busy = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => 0, Listen => 1)
        or die $!;
    my $s = start_server('classes-leak', workers => 1, classes => [qw(a b)]);
    $s->{listen_fault} = 1;
    $s->{class_rejected} = 1;   # only used if the later reload is refused

    $s->{classes} = [qw(a b c)];
    $s->{extra_locations} = 'listen 127.0.0.1:' . $busy->sockport . ';';
    my $mark = length read_file("$s->{root}/error.log");
    $s->{generation}++;
    write_file("$s->{root}/nginx.conf", config($s));
    kill 'HUP', $s->{pid};
    await(sub { substr(read_file("$s->{root}/error.log"), $mark)
                =~ /bind\(\) to 127\.0\.0\.1:\d+ failed \(98: (?:Address already in use|Unknown error)\)/ },
          'the provoked reload failure was not logged', 20);
    $s->{generation}--;
    is(request($s, '/status-a')->{headers}{'x-generation'}, $s->{generation},
       'the failed reload left the old configuration serving');

    $s->{extra_locations} = '';
    $s->{classes} = [qw(a b d)];
    ok(eval { reload_server($s, 1); 1 }, 'a later valid configuration still finds a free slot')
        or diag($@);
    stop_server($s);
};

subtest 'status reply with long class names and many namespaces stays bounded' => sub {
    my @long = map { ('c' x 31) . $_ } qw(a b c);   # three 32-byte names
    my $s = start_server('classes-budget', strict => 1, workers => 1,
                         extra_namespaces => 80, classes => \@long);
    my $r = request($s, '/status-a');
    is($r->{status}, 200, 'classes plus a cut namespace list is still a normal reply');
    cmp_ok(length $r->{content}, '<', 8192, 'the reply stays inside the 8 KiB reply buffer');
    my ($v, $c) = class_status($s);
    is_deeply([ sort keys %$c ], [ sort @long ], 'every class is reported ahead of the namespaces');
    ok(!$v->{namespaces_complete}, 'the cut is still flagged');
    cmp_ok(scalar @{ $v->{namespaces} }, '>=', 40, 'a useful part of the namespace list remains');
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
