use strict;
use warnings;

use File::Path qw(make_path remove_tree);
use File::Spec;
use File::Temp qw(tempdir);
use IO::Socket::INET;
use POSIX qw(:sys_wait_h);
use Test::More;
use Time::HiRes qw(sleep time);

my $nginx = $ENV{TEST_NGINX_BINARY} || 'nginx';
my $prefix = tempdir('ngx-cache-pilot-perm-XXXXXX', TMPDIR => 1, CLEANUP => 1);
my $conf_dir = File::Spec->catdir($prefix, 'conf');
my $logs_dir = File::Spec->catdir($prefix, 'logs');
my $client_temp = File::Spec->catdir($prefix, 'client_temp');
my $proxy_temp = File::Spec->catdir($prefix, 'proxy_temp');
my $cache_dir = File::Spec->catdir($prefix, 'cache');
my $db_path = File::Spec->catfile($prefix, 'cache-pilot.sqlite');
my $conf_path = File::Spec->catfile($conf_dir, 'nginx.conf');
my $port = free_port();
my $pid;

make_path($conf_dir, $logs_dir, $client_temp, $proxy_temp, $cache_dir);

write_config($conf_path, $cache_dir, $proxy_temp, $client_temp, $db_path, $port);

plan tests => 6;

ok(run_nginx_test($nginx, $prefix), 'nginx -t succeeds for sqlite index config');
ok(!-e $db_path, 'nginx -t does not create the sqlite index file');

$pid = fork();
die "fork failed: $!" unless defined $pid;

if ($pid == 0) {
    exec {$nginx} $nginx, '-p', $prefix, '-c', 'conf/nginx.conf';
    die "exec failed: $!";
}

ok(wait_for_port($port), 'nginx runtime starts after config test');
ok(wait_for_file($db_path), 'sqlite index file is created during runtime init');

my $prime = http_request(
    $port,
    "GET /proxy/tagged HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
);
like($prime, qr/^HTTP\/1\.1 200 /m, 'cached origin request succeeds after runtime init');

my $purge = http_request(
    $port,
    "PURGE /proxy/tagged HTTP/1.1\r\nHost: 127.0.0.1\r\nSurrogate-Key: perm-group\r\nX-Purge-Mode: soft\r\nConnection: close\r\n\r\n",
);
like($purge, qr/^HTTP\/1\.1 200 /m, 'purge succeeds using the runtime-created sqlite index');

END {
    if ($pid) {
        kill 'TERM', $pid;
        waitpid($pid, 0);
    }

    if (defined $prefix && -d $prefix) {
        remove_tree($prefix);
    }
}

sub free_port {
    my $sock = IO::Socket::INET->new(
        LocalAddr => '127.0.0.1',
        LocalPort => 0,
        Proto     => 'tcp',
        Listen    => 1,
        ReuseAddr => 1,
    ) or die "failed to allocate port: $!";

    my $port = $sock->sockport();
    close $sock;

    return $port;
}

sub write_config {
    my ($path, $cache, $proxy, $client, $db, $listen_port) = @_;

    open my $fh, '>', $path or die "open $path failed: $!";
    print {$fh} <<"EOF";
worker_processes 1;
daemon off;
master_process off;
error_log logs/error.log info;
pid logs/nginx.pid;

events {
    worker_connections 64;
}

http {
    client_body_temp_path $client;
    proxy_cache_path $cache keys_zone=perm_cache:10m;
    proxy_temp_path $proxy 1 2;

    map \$request_method \$purge_method {
        PURGE 1;
        default 0;
    }

    cache_pilot_index_store sqlite $db;

    server {
        listen 127.0.0.1:$listen_port;

        location = /proxy/tagged {
            proxy_pass http://127.0.0.1:$listen_port/origin/tagged;
            proxy_cache perm_cache;
            proxy_cache_key \$uri\$is_args\$args;
            proxy_cache_valid 3m;
            proxy_cache_purge \$purge_method soft;
            cache_pilot_purge_mode_header X-Purge-Mode;
            cache_pilot_index on;
            add_header X-Cache-Status \$upstream_cache_status;
        }

        location = /origin/tagged {
            add_header Surrogate-Key "perm-group";
            return 200 "origin-tagged";
        }
    }
}
EOF
    close $fh or die "close $path failed: $!";
}

sub run_nginx_test {
    my ($binary, $prefix_dir) = @_;

    my $rc = system {
        $binary
    } $binary, '-p', $prefix_dir, '-c', 'conf/nginx.conf', '-t';

    return $rc == 0;
}

sub wait_for_port {
    my ($listen_port) = @_;
    my $deadline = time + 5;

    while (time < $deadline) {
        my $sock = IO::Socket::INET->new(
            PeerAddr => '127.0.0.1',
            PeerPort => $listen_port,
            Proto    => 'tcp',
            Timeout  => 1,
        );

        if ($sock) {
            close $sock;
            return 1;
        }

        sleep 0.05;
    }

    return 0;
}

sub wait_for_file {
    my ($path) = @_;
    my $deadline = time + 5;

    while (time < $deadline) {
        return 1 if -e $path;
        sleep 0.05;
    }

    return 0;
}

sub http_request {
    my ($listen_port, $request) = @_;

    my $sock = IO::Socket::INET->new(
        PeerAddr => '127.0.0.1',
        PeerPort => $listen_port,
        Proto    => 'tcp',
        Timeout  => 3,
    ) or die "connect failed: $!";

    print {$sock} $request or die "write failed: $!";

    my $response = '';
    while (my $line = <$sock>) {
        $response .= $line;
    }

    close $sock;

    return $response;
}