#!/usr/bin/env perl

use strict;
use warnings;

use FindBin;
use Getopt::Long qw(GetOptions);
use HTTP::Request;
use LWP::UserAgent;
use Time::HiRes qw(sleep);

use lib "$FindBin::Bin/lib";
use Bench qw(hires_time stats_summary write_json);

my %options = (
    count    => 1000,
    duration => 30,
    mode     => 'exact',
    port     => 18080,
    prefix   => '/exact/',
    scenario => 'exact_purge',
    tag_base => 'bench-tag',
);

GetOptions(
    'count=i'    => \$options{count},
    'duration=i' => \$options{duration},
    'mode=s'     => \$options{mode},
    'out=s'      => \$options{out},
    'port=i'     => \$options{port},
    'purge-header=s' => \$options{purge_header},
    'purge-header-value=s' => \$options{purge_header_value},
    'prefix=s'   => \$options{prefix},
    'scenario=s' => \$options{scenario},
    'tag-base=s' => \$options{tag_base},
) or die "invalid arguments\n";

die "--out is required\n" unless defined $options{out};
die "--count must be > 0\n" unless $options{count} > 0;
die "--duration must be > 0\n" unless $options{duration} > 0;

my %interval_by_mode = (
    exact    => 0.1,
    wildcard => 0.5,
    tag      => 0.2,
);

die "unsupported mode: $options{mode}\n"
    unless exists $interval_by_mode{$options{mode}};

my $ua = LWP::UserAgent->new(
    agent      => 'ngx-cache-pilot-bench/1.0',
    keep_alive => 1,
    timeout    => 5,
);

my $base_url = "http://127.0.0.1:$options{port}";
my $deadline = hires_time() + $options{duration};
my $interval = $interval_by_mode{$options{mode}};
my $next_at = hires_time();
my $iteration = 0;
my @latencies;
my %status_codes;

while (hires_time() < $deadline) {
    my $url;
    my $request;

    if ($options{mode} eq 'exact') {
        my $index = int(rand($options{count}));
        $url = $base_url . $options{prefix} . $index;
        $request = HTTP::Request->new('PURGE', $url);

        if (defined $options{purge_header} && length $options{purge_header}
                && defined $options{purge_header_value}) {
            $request->header($options{purge_header} => $options{purge_header_value});
        }

    } elsif ($options{mode} eq 'wildcard') {
        my $digit = $iteration % 10;
        $url = $base_url . $options{prefix} . $digit . '*';
        $request = HTTP::Request->new('PURGE', $url);

    } else {
        my $group = $iteration % 10;
        $url = $base_url . $options{prefix} . '0';
        $request = HTTP::Request->new('PURGE', $url);
        $request->header('Cache-Tag' => "$options{tag_base}-g$group");
    }

    my $started = hires_time();
    my $response = $ua->request($request);
    my $elapsed_us = int((hires_time() - $started) * 1_000_000);
    push @latencies, $elapsed_us;
    $status_codes{$response->code}++;

    $iteration++;
    $next_at += $interval;
    my $sleep_for = $next_at - hires_time();
    sleep($sleep_for) if $sleep_for > 0;
}

write_json($options{out}, {
    scenario     => $options{scenario},
    duration_s   => 0 + $options{duration},
    mode         => $options{mode},
    purge_count  => 0 + $iteration,
    rps          => 0 + sprintf('%.3f', $iteration / $options{duration}),
    status_codes => \%status_codes,
    latency_us   => stats_summary(\@latencies),
});