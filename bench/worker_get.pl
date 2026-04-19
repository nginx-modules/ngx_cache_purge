#!/usr/bin/env perl

use strict;
use warnings;

use File::Path qw(make_path);
use FindBin;
use Getopt::Long qw(GetOptions);
use HTTP::Request;
use JSON::PP ();
use LWP::UserAgent;
use POSIX qw(_exit);

use lib "$FindBin::Bin/lib";
use Bench qw(hires_time stats_summary write_json);

my %options = (
    concurrency => 50,
    count       => 1000,
    duration    => 30,
    port        => 18080,
    prefix      => '/exact/',
    scenario    => 'exact_get',
    scratch_dir => '/tmp',
);

GetOptions(
    'concurrency=i' => \$options{concurrency},
    'count=i'       => \$options{count},
    'duration=i'    => \$options{duration},
    'out=s'         => \$options{out},
    'port=i'        => \$options{port},
    'prefix=s'      => \$options{prefix},
    'scenario=s'    => \$options{scenario},
    'scratch-dir=s' => \$options{scratch_dir},
    'vary-header=s' => \$options{vary_header},
    'vary-values=s' => \$options{vary_values},
) or die "invalid arguments\n";

die "--out is required\n" unless defined $options{out};
die "--concurrency must be > 0\n" unless $options{concurrency} > 0;
die "--count must be > 0\n" unless $options{count} > 0;
die "--duration must be > 0\n" unless $options{duration} > 0;

my $base_url = "http://127.0.0.1:$options{port}";
my $end_time = hires_time() + $options{duration};
my @children;
my @vary_values = defined $options{vary_values}
    ? grep { length $_ } split /,/, $options{vary_values}
    : ();

make_path($options{scratch_dir}) unless -d $options{scratch_dir};

for (1 .. $options{concurrency}) {
    my $temp_file = sprintf('%s/bench_get_child_%d_%d.jsonl',
        $options{scratch_dir}, $$, $_);
    my $pid = fork();

    die "fork failed: $!\n" unless defined $pid;

    if ($pid == 0) {
        my $encoder = JSON::PP->new->ascii;
        my $ua = LWP::UserAgent->new(
            agent      => 'ngx-cache-pilot-bench/1.0',
            keep_alive => 5,
            timeout    => 5,
        );

        open my $fh, '>', $temp_file or die "open($temp_file): $!";

        while (hires_time() < $end_time) {
            my $index = int(rand($options{count}));
            my $url = $base_url . $options{prefix} . $index;
            my $request = HTTP::Request->new('GET', $url);

            if (@vary_values && defined $options{vary_header} && length $options{vary_header}) {
                my $vary_value = $vary_values[$index % @vary_values];
                $request->header($options{vary_header} => $vary_value);
            }

            my $started = hires_time();
            my $response = $ua->request($request);
            my $elapsed_us = int((hires_time() - $started) * 1_000_000);
            my $cache_status = $response->header('X-Cache-Status');
            $cache_status = defined $cache_status && length $cache_status
                ? uc $cache_status
                : 'UNKNOWN';

            print {$fh} $encoder->encode({
                latency_us    => $elapsed_us,
                status_code   => 0 + $response->code,
                x_cache_status => $cache_status,
            }), "\n" or die "write($temp_file): $!";
        }

        close $fh or die "close($temp_file): $!";
        _exit(0);
    }

    push @children, {
        pid  => $pid,
        file => $temp_file,
    };
}

for my $child (@children) {
    waitpid($child->{pid}, 0);
    die "child $child->{pid} failed\n" if $? != 0;
}

my @latencies;
my %status_codes;
my %x_cache_breakdown;
my $requests = 0;

for my $child (@children) {
    open my $fh, '<', $child->{file} or die "open($child->{file}): $!";
    while (my $line = <$fh>) {
        my $sample = JSON::PP::decode_json($line);
        push @latencies, $sample->{latency_us};
        $status_codes{$sample->{status_code}}++;
        $x_cache_breakdown{$sample->{x_cache_status}}++;
        $requests++;
    }
    close $fh or die "close($child->{file}): $!";
    unlink $child->{file} or die "unlink($child->{file}): $!";
}

my $hit_rate = $requests ? (($x_cache_breakdown{HIT} || 0) / $requests) : 0;

write_json($options{out}, {
    scenario          => $options{scenario},
    duration_s        => 0 + $options{duration},
    requests          => 0 + $requests,
    rps               => 0 + sprintf('%.3f', $requests / $options{duration}),
    cache_hit_rate    => 0 + sprintf('%.4f', $hit_rate),
    x_cache_breakdown => \%x_cache_breakdown,
    status_codes      => \%status_codes,
    latency_us        => stats_summary(\@latencies),
});