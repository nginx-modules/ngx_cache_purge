package Bench;

use strict;
use warnings;

use Exporter qw(import);
use JSON::PP ();
use LWP::Simple ();
use Time::HiRes qw(gettimeofday);

our @EXPORT_OK = qw(
    fetch_stats
    format_table
    hires_time
    percentile
    stats_delta
    stats_summary
    write_json
);

sub hires_time {
    my ($seconds, $microseconds) = gettimeofday();
    return $seconds + ($microseconds / 1_000_000);
}

sub percentile {
    my ($sorted_values, $percentile) = @_;

    return 0 unless @{$sorted_values};
    return $sorted_values->[0] if @{$sorted_values} == 1;

    my $rank = ($percentile / 100) * (@{$sorted_values} - 1);
    my $lower_index = int($rank);
    my $upper_index = $lower_index >= $#{$sorted_values}
        ? $#{$sorted_values}
        : $lower_index + 1;

    return $sorted_values->[$lower_index] if $lower_index == $upper_index;

    my $fraction = $rank - $lower_index;
    return $sorted_values->[$lower_index]
        + (($sorted_values->[$upper_index] - $sorted_values->[$lower_index])
            * $fraction);
}

sub stats_summary {
    my ($latencies_us) = @_;

    return {
        min  => 0,
        p50  => 0,
        p95  => 0,
        p99  => 0,
        max  => 0,
        mean => 0,
    } unless @{$latencies_us};

    my @sorted = sort { $a <=> $b } @{$latencies_us};
    my $sum = 0;
    $sum += $_ for @sorted;

    return {
        min  => 0 + sprintf('%.0f', $sorted[0]),
        p50  => 0 + sprintf('%.0f', percentile(\@sorted, 50)),
        p95  => 0 + sprintf('%.0f', percentile(\@sorted, 95)),
        p99  => 0 + sprintf('%.0f', percentile(\@sorted, 99)),
        max  => 0 + sprintf('%.0f', $sorted[-1]),
        mean => 0 + sprintf('%.2f', $sum / @sorted),
    };
}

sub write_json {
    my ($file, $data) = @_;

    open my $fh, '>', $file or die "open($file): $!";
    print {$fh} JSON::PP->new->ascii->canonical->pretty->encode($data)
        or die "write($file): $!";
    close $fh or die "close($file): $!";
}

sub fetch_stats {
    my ($url) = @_;

    my $body = LWP::Simple::get($url);
    die "failed to fetch $url\n" unless defined $body;

    return JSON::PP::decode_json($body);
}

sub stats_delta {
    my ($before, $after) = @_;

    return undef if !defined $after;

    if (ref($after) eq 'HASH') {
        my %delta;
        for my $key (sort keys %{$after}) {
            my $value = stats_delta(
                ref($before) eq 'HASH' ? $before->{$key} : undef,
                $after->{$key},
            );
            next if !defined $value;
            $delta{$key} = $value;
        }
        return \%delta;
    }

    if (ref($after) eq 'ARRAY') {
        return [@{$after}];
    }

    if (!ref($after) && defined $after && $after =~ /^-?(?:\d+(?:\.\d+)?|\.\d+)$/) {
        my $before_value = 0;
        if (defined $before && !ref($before)
                && $before =~ /^-?(?:\d+(?:\.\d+)?|\.\d+)$/) {
            $before_value = $before;
        }
        return 0 + ($after - $before_value);
    }

    return undef;
}

sub _format_milliseconds {
    my ($microseconds) = @_;
    return sprintf('%.1fms', $microseconds / 1000);
}

sub _format_hit_percent {
    my ($ratio) = @_;
    return sprintf('%.1f%%', ($ratio || 0) * 100);
}

sub format_table {
    my ($results) = @_;
    my @lines = (
        sprintf('%-22s %8s %7s %7s %7s %6s %7s %8s %9s',
            'Scenario', 'GET rps', 'p50', 'p95', 'p99', 'Hit%', 'Purges',
            'IdxPlan', 'IdxSeen'),
        '-' x 98,
    );

    for my $result (@{$results}) {
        my $index_plan = defined $result->{table_index_plan}
            ? $result->{table_index_plan}
            : '-';
        my $index_seen = defined $result->{table_index_observed}
            ? $result->{table_index_observed}
            : '-';

        push @lines, sprintf('%-22s %8.1f %7s %7s %7s %6s %7d %8s %9s',
            $result->{table_name},
            $result->{get}->{rps} || 0,
            _format_milliseconds($result->{get}->{latency_us}->{p50} || 0),
            _format_milliseconds($result->{get}->{latency_us}->{p95} || 0),
            _format_milliseconds($result->{get}->{latency_us}->{p99} || 0),
            _format_hit_percent($result->{get}->{cache_hit_rate}),
            $result->{purge}->{purge_count} || 0,
            $index_plan,
            $index_seen,
        );
    }

    return join("\n", @lines) . "\n";
}

1;