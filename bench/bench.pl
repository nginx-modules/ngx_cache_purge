#!/usr/bin/env perl

use strict;
use warnings;

use File::Basename qw(dirname);
use File::Path qw(make_path remove_tree);
use File::Temp qw(tempdir);
use FindBin;
use Getopt::Long qw(GetOptions);
use HTTP::Request;
use JSON::PP ();
use LWP::UserAgent;
use POSIX qw(setpgid strftime);
use Time::HiRes qw(sleep);

use lib "$FindBin::Bin/lib";
use Bench qw(fetch_stats format_table hires_time stats_delta write_json);

my %options = (
    count       => 1000,
    concurrency => 50,
    out_dir     => "$FindBin::Bin/results",
    port        => 18080,
    scenario_set => 'default',
);

GetOptions(
    'assert-file=s' => \$options{assert_file},
    'count=i'       => \$options{count},
    'config-template=s' => \$options{config_template},
    'concurrency=i' => \$options{concurrency},
    'out-dir=s'     => \$options{out_dir},
    'port=i'        => \$options{port},
    'quick'         => \$options{quick},
    'scenario-set=s' => \$options{scenario_set},
    'scenarios=s'   => \$options{scenarios},
) or die "invalid arguments\n";

die "--count must be > 0\n" unless $options{count} > 0;
die "--concurrency must be > 0\n" unless $options{concurrency} > 0;

my $duration = $options{quick} ? 15 : 60;
my $index_probe_timeout_s = 15;
my $scenario_ready_timeout_s = 10;
my $perl = $^X;
my $nginx = '/opt/nginx/sbin/nginx';
my $get_worker = "$FindBin::Bin/worker_get.pl";
my $purge_worker = "$FindBin::Bin/worker_purge.pl";
my $redis_port = 16379;
my $redis_db = 5;
my $stats_url = "http://127.0.0.1:$options{port}/bench_stats";
my $ua = LWP::UserAgent->new(agent => 'ngx-cache-pilot-bench/1.0', keep_alive => 5, timeout => 5);
my $runtime_root;
my $runtime_conf_dir;
my $worker_scratch_dir;
my $nginx_prefix;
my $nginx_error_log;
my $nginx_pid_file;
my $redis_runtime_dir;
my $redis_pid_file;
my $redis_log_file;
my $sqlite_runtime_dir;
my $sqlite_db_path;

die "nginx binary not found at $nginx; run make nginx-build first\n"
    unless -x $nginx;

my %config_templates = discover_config_templates($FindBin::Bin);
my $override_template = defined $options{config_template}
    ? resolve_config_template(\%config_templates, $options{config_template})
    : undef;

my @all_scenarios = (
    {
        key        => 'exact',
        name       => 'exact_purge',
        table_name => 'exact_purge',
        config_template => 'nginx',
        prefix     => '/exact/',
        mode       => 'exact',
        backend    => 'sqlite',
        index_tracking_mode => 'disabled',
    },
    {
        key        => 'wild',
        name       => 'wildcard_purge',
        table_name => 'wildcard_purge',
        config_template => 'nginx',
        prefix     => '/wild/',
        mode       => 'wildcard',
        backend    => 'sqlite',
        index_tracking_mode => 'disabled',
    },
    {
        key        => 'tag-sqlite',
        name       => 'tag_sqlite_purge',
        table_name => 'tag_sqlite_purge',
        config_template => 'nginx',
        prefix     => '/tag/',
        mode       => 'tag',
        tag_base   => 'bench-tag',
        backend    => 'sqlite',
        index_target_zone => 'bench_tag_sqlite',
        require_index_zone_ready => 0,
        index_tracking_mode => 'readiness_only',
    },
    {
        key        => 'tag-redis',
        name       => 'tag_redis_purge',
        table_name => 'tag_redis_purge',
        config_template => 'nginx_redis',
        prefix     => '/rtag/',
        mode       => 'tag',
        tag_base   => 'bench-rtag',
        backend    => 'redis',
        index_target_zone => 'bench_tag_redis',
        require_index_zone_ready => 0,
        index_tracking_mode => 'readiness_only',
    },
    {
        key        => 'exact-scan',
        name       => 'exact_scan_purge',
        table_name => 'exact_scan_purge',
        config_template => 'nginx',
        prefix     => '/exact-scan/',
        mode       => 'exact',
        backend    => 'sqlite',
        index_tracking_mode => 'disabled',
    },
    {
        key        => 'exact-index',
        name       => 'exact_indexed_purge',
        table_name => 'exact_indexed_purge',
        config_template => 'nginx',
        prefix     => '/exact-index/',
        mode       => 'exact',
        backend    => 'sqlite',
        index_target_zone => 'bench_exact_index',
        require_index_zone_ready => 1,
        index_tracking_mode => 'exact_fanout',
        vary_header => 'X-Bench-Variant',
        vary_values => [qw(a b c)],
        purge_header => 'X-Bench-Variant',
        purge_header_value => 'a',
    },
    {
        key        => 'wild-scan',
        name       => 'wildcard_scan_purge',
        table_name => 'wildcard_scan_purge',
        config_template => 'nginx',
        prefix     => '/wild-scan/',
        mode       => 'wildcard',
        backend    => 'sqlite',
        index_tracking_mode => 'disabled',
    },
    {
        key        => 'wild-index',
        name       => 'wildcard_indexed_purge',
        table_name => 'wildcard_indexed_purge',
        config_template => 'nginx',
        prefix     => '/wild-index/',
        mode       => 'wildcard',
        backend    => 'sqlite',
        index_target_zone => 'bench_wild_index',
        require_index_zone_ready => 1,
        index_tracking_mode => 'wildcard_prefix',
    },
);

my %scenario_sets = (
    default => [ map { $_->{key} } @all_scenarios ],
    all => [ map { $_->{key} } @all_scenarios ],
);

my %scenario_by_key = map { $_->{key} => $_ } @all_scenarios;
my @selected_keys;
my $selection_mode;

if (defined $options{scenarios}) {
    @selected_keys = grep { length $_ } split(/,/, $options{scenarios});
    $selection_mode = 'scenarios';
} else {
    die "unknown scenario set: $options{scenario_set}\n"
        unless exists $scenario_sets{$options{scenario_set}};
    @selected_keys = @{ $scenario_sets{$options{scenario_set}} };
    $selection_mode = 'scenario-set';
}

die "no scenarios selected\n" unless @selected_keys;

my @scenarios;
for my $key (@selected_keys) {
    die "unknown scenario: $key\n" unless exists $scenario_by_key{$key};
    my %scenario = %{ $scenario_by_key{$key} };
    $scenario{config_template_path} = defined $override_template
        ? $override_template->{path}
        : resolve_config_template(\%config_templates, $scenario{config_template})->{path};
    $scenario{config_template_name} = defined $override_template
        ? $override_template->{name}
        : $scenario{config_template};
    push @scenarios, \%scenario;
}

my $current_conf;
my $redis_started = 0;
my @active_child_pids;
my $shutdown_signal;
my $shutting_down = 0;
my $nginx_error_log_offset = 0;
my $pending_nginx_error_log = '';

$SIG{INT} = sub { handle_signal('INT'); };
$SIG{TERM} = sub { handle_signal('TERM'); };

END {
    cleanup_active_children();
    eval { stop_nginx($current_conf) if defined $current_conf; };
    eval { stop_redis() if $redis_started; };
    eval { cleanup_runtime_root(); };
}

prepare_output_dir($options{out_dir});
my $timestamp = strftime('%Y%m%d-%H%M%S', localtime());
my $run_dir = "$options{out_dir}/$timestamp";
make_path($run_dir);
ensure_empty_file("$run_dir/nginx_error.log");
initialize_runtime_paths($run_dir);
log_info('Preparing benchmark runtime state');
cleanup_runtime_state();
log_info("Writing run artifacts to $run_dir");

my @results;
my $active_runtime;
my $active_scenario;
my $failure;

eval {
    for my $scenario (@scenarios) {
        $active_scenario = $scenario;

        my $runtime_key = join("\0",
            $scenario->{config_template_path},
            $scenario->{backend},
        );

        if (!defined $active_runtime || $active_runtime ne $runtime_key) {
            log_info(sprintf(
                'Switching runtime: template=%s backend=%s',
                $scenario->{config_template_name},
                $scenario->{backend},
            ));
            stop_nginx($current_conf) if defined $current_conf;
            $current_conf = undef;

            if ($redis_started) {
                log_info('Stopping Redis sidecar');
                stop_redis();
                $redis_started = 0;
            }

            cleanup_runtime_state();

            if ($scenario->{backend} eq 'redis') {
                log_info('Starting Redis sidecar');
                start_redis();
                $redis_started = 1;
            }

            $current_conf = render_runtime_config($scenario);

            log_info("Starting nginx with $current_conf");
            start_nginx($current_conf);
            wait_for_stats($stats_url);
            log_info('Metrics endpoint is ready');
            record_nginx_error_log($run_dir, join('_', $scenario->{config_template_name}, $scenario->{backend}, 'startup'));
            $active_runtime = $runtime_key;
        }

        log_info(sprintf(
            'Running scenario %s (%ss)',
            $scenario->{name},
            $duration,
        ));
        my $result = run_scenario($scenario, $duration, $run_dir, $stats_url);
        push @results, $result;
        log_info(sprintf(
            'Completed scenario %s: GET rps=%.1f purge_count=%d',
            $scenario->{name},
            $result->{get}->{rps} || 0,
            $result->{purge}->{purge_count} || 0,
        ));
    }
    1;
} or do {
    my $error = $@ || 'benchmark failed';
    chomp $error;
    $failure = {
        message => $error,
        scenario => defined $active_scenario ? $active_scenario->{key} : undef,
        name => defined $active_scenario ? $active_scenario->{name} : undef,
    };

    eval {
        my $label = defined $active_scenario
            ? $active_scenario->{name} . '_failure'
            : 'run_failure';
        record_nginx_error_log($run_dir, $label);
    };
};

stop_nginx($current_conf) if defined $current_conf;
$current_conf = undef;

if ($redis_started) {
    log_info('Stopping Redis sidecar');
    stop_redis();
    $redis_started = 0;
}

my $summary = {
    generated_at => $timestamp,
    quick        => $options{quick} ? JSON::PP::true : JSON::PP::false,
    completed    => $failure ? JSON::PP::false : JSON::PP::true,
    duration_s   => $duration,
    count        => $options{count},
    concurrency  => $options{concurrency},
    port         => $options{port},
    scenario_selection => {
        mode => $selection_mode,
        scenario_set => defined $options{scenarios} ? 'custom' : $options{scenario_set},
        keys => \@selected_keys,
    },
    scenarios    => \@results,
};

if ($failure) {
    $summary->{failure} = $failure;
}

if (defined $options{assert_file} && !$failure) {
    log_info("Evaluating assertions from $options{assert_file}");
    my @violations = evaluate_assertions($summary, $options{assert_file});
    $summary->{assertions} = {
        file       => $options{assert_file},
        passed     => @violations ? JSON::PP::false : JSON::PP::true,
        violations => \@violations,
    };
}

write_json("$run_dir/summary.json", $summary);
my $table = format_table(\@results);
open my $summary_fh, '>', "$run_dir/summary.txt" or die "open(summary.txt): $!";
print {$summary_fh} $table or die "write(summary.txt): $!";
close $summary_fh or die "close(summary.txt): $!";
log_info('Wrote summary artifacts');

update_latest_symlink($options{out_dir}, $timestamp);
log_info('Updated latest symlink');
print $table;
print_summary_json($summary);

if (defined $summary->{assertions}) {
    print_assertion_result($summary->{assertions});
    exit 1 unless $summary->{assertions}->{passed};
}

if ($failure) {
    die "$failure->{message}\n";
}

sub run_scenario {
    my ($scenario, $duration_s, $run_dir, $stats_endpoint) = @_;
    my $before;
    my $probe_report;

    wait_for_scenario_ready($scenario, $stats_endpoint, $scenario_ready_timeout_s);
    log_info("Warming cache for $scenario->{name}");
    warm_cache($scenario->{prefix}, $options{count}, $scenario);

    if (($scenario->{index_tracking_mode} || '') eq 'wildcard_prefix') {
        # Wildcard index metadata is written asynchronously; give it a brief
        # settle window before running wildcard preflight probes.
        sleep(1.0);
    }

    $probe_report = ensure_index_probe_ready($scenario, $stats_endpoint,
                                             $index_probe_timeout_s);

    log_info("Fetching baseline stats for $scenario->{name}");
    $before = fetch_stats($stats_endpoint);

    my $get_out = "$run_dir/$scenario->{name}_get.json";
    my $purge_out = "$run_dir/$scenario->{name}_purge.json";

    log_info("Launching GET workers for $scenario->{name}");
    my $get_pid = fork_exec(
        $perl,
        $get_worker,
        '--scenario', $scenario->{name} . '_get',
        '--prefix', $scenario->{prefix},
        '--count', $options{count},
        '--concurrency', $options{concurrency},
        '--duration', $duration_s,
        '--port', $options{port},
        '--out', $get_out,
        '--scratch-dir', $worker_scratch_dir,
        (defined $scenario->{vary_header}
            ? ('--vary-header', $scenario->{vary_header})
            : ()),
        (defined $scenario->{vary_values}
            ? ('--vary-values', join(',', @{ $scenario->{vary_values} }))
            : ()),
    );

    sleep(2);

    log_info("Launching PURGE worker for $scenario->{name}");
    my @purge_command = (
        $perl,
        $purge_worker,
        '--scenario', $scenario->{name},
        '--mode', $scenario->{mode},
        '--prefix', $scenario->{prefix},
        '--count', $options{count},
        '--duration', $duration_s,
        '--port', $options{port},
        '--out', $purge_out,
    );

    if (defined $scenario->{tag_base}) {
        push @purge_command, '--tag-base', $scenario->{tag_base};
    }

    if (defined $scenario->{purge_header}) {
        push @purge_command, '--purge-header', $scenario->{purge_header};
    }

    if (defined $scenario->{purge_header_value}) {
        push @purge_command, '--purge-header-value', $scenario->{purge_header_value};
    }

    my $purge_pid = fork_exec(@purge_command);

    log_info("Waiting for workers to finish for $scenario->{name}");
    wait_for_child($get_pid, 'GET worker');
    wait_for_child($purge_pid, 'PURGE worker');

    log_info("Fetching final stats for $scenario->{name}");
    my $after = fetch_stats($stats_endpoint);
    my $get_result = read_json($get_out);
    my $purge_result = read_json($purge_out);
    my $metrics_delta = stats_delta($before, $after);
    my $nginx_log = record_nginx_error_log($run_dir, $scenario->{name});
    my $index_plan = scenario_index_plan($scenario);
    my $index_report = build_index_report($scenario, $before, $after,
                                          $metrics_delta, $nginx_log,
                                          $probe_report);

    my $scenario_result = {
        scenario      => $scenario->{key},
        name          => $scenario->{name},
        table_name    => $scenario->{table_name},
        backend       => $scenario->{backend},
        config_template => $scenario->{config_template_name},
        duration_s    => $duration_s,
        get           => $get_result,
        purge         => $purge_result,
        stats_before  => $before,
        stats_after   => $after,
        metrics_delta => $metrics_delta,
        index_plan    => $index_plan,
        index_report  => $index_report,
        table_index_plan => $index_plan->{short_label},
        table_index_observed => $index_report->{short_label},
        nginx_error_log => $nginx_log,
    };

    write_json("$run_dir/$scenario->{name}.json", $scenario_result);
    unlink $get_out or die "unlink($get_out): $!";
    unlink $purge_out or die "unlink($purge_out): $!";

    return $scenario_result;
}

sub prepare_output_dir {
    my ($out_dir) = @_;
    make_path($out_dir) unless -d $out_dir;
}

sub initialize_runtime_paths {
    my ($run_dir) = @_;

    $runtime_root = tempdir('ngx-cache-pilot-bench-XXXXXX', TMPDIR => 1, CLEANUP => 0);
    $runtime_conf_dir = "$runtime_root/conf";
    $worker_scratch_dir = "$runtime_root/workers";
    $sqlite_runtime_dir = "$runtime_root/sqlite";
    $sqlite_db_path = "$sqlite_runtime_dir/bench_tags.db";
    $nginx_prefix = "$runtime_root/nginx";
    $nginx_error_log = "$nginx_prefix/logs/error.log";
    $nginx_pid_file = "$nginx_prefix/nginx.pid";
    $redis_runtime_dir = "$runtime_root/redis";
    $redis_pid_file = "$redis_runtime_dir/redis.pid";
    $redis_log_file = "$redis_runtime_dir/redis.log";
}

sub cleanup_runtime_state {
    if (defined $runtime_root && -e $runtime_root) {
        remove_tree($runtime_root, { error => \my $error });
        if ($error && @{$error}) {
            for my $entry (@{$error}) {
                my ($failed_path, $message) = %{$entry};
                die "remove_tree($failed_path): $message\n" if defined $message;
            }
        }
    }

    $nginx_error_log_offset = 0;
    $pending_nginx_error_log = '';

    make_path(
        $runtime_conf_dir,
        $worker_scratch_dir,
        $sqlite_runtime_dir,
        "$runtime_root/proxy_temp",
        "$runtime_root/client_body_temp",
        "$runtime_root/cache_exact",
        "$runtime_root/cache_wild",
        "$runtime_root/cache_exact_scan",
        "$runtime_root/cache_exact_index",
        "$runtime_root/cache_wild_scan",
        "$runtime_root/cache_wild_index",
        "$runtime_root/cache_fanout",
        "$runtime_root/cache_tag_sqlite",
        "$runtime_root/cache_tag_redis",
        $nginx_prefix,
        "$nginx_prefix/logs",
        $redis_runtime_dir,
    );

    prepare_sqlite_runtime();
    ensure_nginx_prefix();
}

sub prepare_sqlite_runtime {
    chmod 0777, $sqlite_runtime_dir
        or die "chmod($sqlite_runtime_dir): $!\n";

    open my $fh, '>', $sqlite_db_path or die "open($sqlite_db_path): $!";
    close $fh or die "close($sqlite_db_path): $!";

    chmod 0666, $sqlite_db_path
        or die "chmod($sqlite_db_path): $!\n";
}

sub render_runtime_config {
    my ($scenario) = @_;

    my $backend = $scenario->{backend};
    my $template_name = sanitize_config_name($scenario->{config_template_name});
    my $target = "$runtime_conf_dir/bench_nginx_${template_name}_${backend}.conf";

    render_config($scenario->{config_template_path}, $target, $options{port});

    return $target;
}

sub render_config {
    my ($source, $target, $port) = @_;

    open my $in, '<', $source or die "open($source): $!";
    local $/;
    my $config = <$in>;
    close $in or die "close($source): $!";

    $config =~ s/listen\s+18080;/listen $port;/;

    my %path_map = (
        '/tmp/bench_nginx_error.log' => $nginx_error_log,
        '/tmp/bench_nginx.pid' => $nginx_pid_file,
        '/tmp/bench_client_body_temp' => "$runtime_root/client_body_temp",
        '/tmp/bench_proxy_temp' => "$runtime_root/proxy_temp",
        '/tmp/bench_cache_exact' => "$runtime_root/cache_exact",
        '/tmp/bench_cache_wild' => "$runtime_root/cache_wild",
        '/tmp/bench_cache_exact_scan' => "$runtime_root/cache_exact_scan",
        '/tmp/bench_cache_exact_index' => "$runtime_root/cache_exact_index",
        '/tmp/bench_cache_wild_scan' => "$runtime_root/cache_wild_scan",
        '/tmp/bench_cache_wild_index' => "$runtime_root/cache_wild_index",
        '/tmp/bench_cache_fanout' => "$runtime_root/cache_fanout",
        '/tmp/bench_cache_tag_sqlite' => "$runtime_root/cache_tag_sqlite",
        '/tmp/bench_cache_tag_redis' => "$runtime_root/cache_tag_redis",
        '/tmp/bench_tags.db' => $sqlite_db_path,
    );

    for my $from (sort { length($b) <=> length($a) } keys %path_map) {
        my $to = $path_map{$from};
        $config =~ s/\Q$from\E/$to/g;
    }

    open my $out, '>', $target or die "open($target): $!";
    print {$out} $config or die "write($target): $!";
    close $out or die "close($target): $!";
}

sub start_nginx {
    my ($conf) = @_;

    ensure_nginx_prefix();
    run_system($nginx, '-p', $nginx_prefix, '-c', $conf);
}

sub stop_nginx {
    my ($conf) = @_;

    return unless defined $conf;
    return unless defined $nginx_pid_file && -e $nginx_pid_file;

    ensure_nginx_prefix();
    system($nginx, '-p', $nginx_prefix, '-c', $conf, '-s', 'stop');
    sleep(0.2);
    unlink $nginx_pid_file if -e $nginx_pid_file;
}

sub ensure_nginx_prefix {
    return unless defined $nginx_prefix;

    make_path($nginx_prefix, "$nginx_prefix/logs");

    return if -e $nginx_error_log;

    open my $fh, '>', $nginx_error_log or die "open($nginx_error_log): $!";
    close $fh or die "close($nginx_error_log): $!";
}

sub wait_for_stats {
    my ($url) = @_;

    for (1 .. 100) {
        eval {
            my $payload = fetch_stats($url);
            die "stats payload missing zones\n" unless ref($payload->{zones}) eq 'HASH';
        };
        return if !$@;

        my $startup_failure = detect_nginx_startup_failure();
        die "$startup_failure\n" if defined $startup_failure;

        sleep(0.2);
    }

    die "nginx did not become ready at $url\n";
}

sub cleanup_runtime_root {
    return unless defined $runtime_root && -e $runtime_root;

    remove_tree($runtime_root, { error => \my $error });
    if ($error && @{$error}) {
        for my $entry (@{$error}) {
            my ($failed_path, $message) = %{$entry};
            die "remove_tree($failed_path): $message\n" if defined $message;
        }
    }
}

sub detect_nginx_startup_failure {
    my $chunk = stash_nginx_error_log_chunk();
    my @lines = grep { length $_ } split /\n/, $chunk;

    for my $line (@lines) {
        if ($line =~ /\[emerg\]/
                || $line =~ /cannot be respawned/
                || $line =~ /exited with fatal code/
                || $line =~ /sqlite (?:prepare|exec) failed/
                || $line =~ /attempt to write a readonly database/
                || $line =~ /no such table/) {
            return "nginx startup failed: $line";
        }
    }

    if (defined $nginx_pid_file && -e $nginx_pid_file) {
        my $pid = read_pid_file($nginx_pid_file);
        if (defined $pid && $pid > 0 && !kill(0, $pid)) {
            return "nginx master exited during startup";
        }
    }

    return undef;
}

sub read_pid_file {
    my ($file) = @_;

    open my $fh, '<', $file or return undef;
    my $pid = <$fh>;
    close $fh;

    return undef unless defined $pid;
    chomp $pid;
    return undef unless $pid =~ /^\d+$/;

    return 0 + $pid;
}

sub warm_cache {
    my ($prefix, $count, $scenario) = @_;

    my @vary_values;
    my $vary_header;
    if (defined $scenario && ref($scenario->{vary_values}) eq 'ARRAY') {
        @vary_values = @{ $scenario->{vary_values} };
        $vary_header = $scenario->{vary_header};
    }

    for my $index (0 .. ($count - 1)) {
        if (@vary_values > 0 && defined $vary_header && length $vary_header) {
            for my $vary_value (@vary_values) {
                my $response = $ua->get(
                    "http://127.0.0.1:$options{port}$prefix$index",
                    $vary_header => $vary_value,
                );
                die "warm-up failed for $prefix$index [$vary_header=$vary_value]: "
                    . $response->status_line . "\n"
                    unless $response->is_success;
            }
            next;
        }

        my $response = $ua->get("http://127.0.0.1:$options{port}$prefix$index");
        die "warm-up failed for $prefix$index: " . $response->status_line . "\n"
            unless $response->is_success;
    }

    sleep(0.5);
}

sub ensure_index_probe_ready {
    my ($scenario, $stats_url, $timeout_s) = @_;
    my $mode = $scenario->{index_tracking_mode} || 'disabled';
    my $probe_prefix;
    my @probe_urls;
    my $purge_url;
    my $before;
    my $after;
    my $delta;
    my $request;
    my $response;
    my $deadline;
    my $attempts = 0;
    my $last_hits = 0;

    return {
        attempted => 0,
        succeeded => 0,
    } unless $mode eq 'wildcard_prefix';

    $probe_prefix = $options{count};
    $deadline = hires_time() + $timeout_s;
    @probe_urls = map {
        "http://127.0.0.1:$options{port}$scenario->{prefix}${probe_prefix}$_"
    } 0 .. 9;
    $purge_url = "http://127.0.0.1:$options{port}$scenario->{prefix}${probe_prefix}*";

    log_info("Running wildcard index preflight for $scenario->{name}");

    while (hires_time() < $deadline) {
        $attempts++;

        for my $probe_url (@probe_urls) {
            $response = $ua->get($probe_url);
            die "wildcard index preflight warm-up failed for $probe_url: "
                . $response->status_line . "\n"
                unless $response->is_success;
        }

        sleep(0.3);

        $before = fetch_stats($stats_url);
        $request = HTTP::Request->new('PURGE', $purge_url);
        $response = $ua->request($request);
        die "wildcard index preflight purge failed for $purge_url: "
            . $response->status_line . "\n"
            unless $response->is_success;

        sleep(0.3);

        $after = fetch_stats($stats_url);
        $delta = stats_delta($before, $after);
        $last_hits = key_index_counter_delta($delta, 'wildcard_hits');

        if ($last_hits > 0) {
            return {
                attempted => 1,
                succeeded => 1,
                attempts => $attempts,
                probe_key_count => scalar @probe_urls,
                observed_hits => $last_hits,
            };
        }
    }

    log_info(sprintf(
        'wildcard index preflight timed out for %s after %ss: metadata never produced a wildcard index hit',
        $scenario->{name},
        $timeout_s,
    ));

    return {
        attempted => 1,
        succeeded => 0,
        attempts => $attempts,
        probe_key_count => scalar @probe_urls,
        observed_hits => $last_hits,
    };
}

sub wait_for_scenario_ready {
    my ($scenario, $stats_url, $timeout_s) = @_;
    my $deadline = hires_time() + $timeout_s;
    my @requests = scenario_ready_requests($scenario);
    my $last_status = 'not attempted';

    while (hires_time() < $deadline) {
        my $all_ready = 1;

        for my $request (@requests) {
            my $response = $ua->get($request->{url}, @{ $request->{headers} });
            if (!$response->is_success) {
                $all_ready = 0;
                $last_status = $response->status_line;
                last;
            }
        }

        if ($all_ready && scenario_ready_state($scenario, $stats_url)) {
            return;
        }

        sleep(0.2);
    }

    die sprintf(
        'scenario readiness timed out for %s after %ss: %s' . "\n",
        $scenario->{name},
        $timeout_s,
        $last_status,
    );
}

sub scenario_ready_state {
    my ($scenario, $stats_url) = @_;
    my $zone;
    my $state;
    my $stats;

    return 1 unless $scenario->{require_index_zone_ready};
    return 1 unless defined $scenario->{index_target_zone}
                    && length $scenario->{index_target_zone};

    $stats = fetch_stats($stats_url);
    $zone = $stats->{zones}->{$scenario->{index_target_zone}};
    return 0 unless ref($zone) eq 'HASH';

    $state = $zone->{index}->{state_code};
    return defined $state && $state == 2;
}

sub scenario_index_plan {
    my ($scenario) = @_;
    my $mode = $scenario->{index_tracking_mode} || 'disabled';
    my $zone = defined $scenario->{index_target_zone}
               ? $scenario->{index_target_zone} : '';

    return {
        tracking_mode   => $mode,
        target_zone     => $zone,
        ready_gate      => $scenario->{require_index_zone_ready} ? 1 : 0,
        expected_assist => ($mode eq 'wildcard_prefix' || $mode eq 'exact_fanout')
                           ? 1 : 0,
        short_label     => index_plan_short_label($mode),
    };
}

sub build_index_report {
    my ($scenario, $before, $after, $metrics_delta, $nginx_log, $probe_report) = @_;
    my $mode = $scenario->{index_tracking_mode} || 'disabled';
    my $zone = $scenario->{index_target_zone};
    my $before_state = zone_index_state_code($before, $zone);
    my $after_state = zone_index_state_code($after, $zone);
    my $wildcard_hits = key_index_counter_delta($metrics_delta, 'wildcard_hits');
    my $exact_fanout = key_index_counter_delta($metrics_delta, 'exact_fanout');
    my $used_assist = 0;
    my $expected_assist = ($mode eq 'wildcard_prefix' || $mode eq 'exact_fanout') ? 1 : 0;

    if ($mode eq 'wildcard_prefix' && $wildcard_hits > 0) {
        $used_assist = 1;
    }

    if ($mode eq 'exact_fanout' && $exact_fanout > 0) {
        $used_assist = 1;
    }

    my $ready_before = defined $before_state && $before_state == 2 ? 1 : 0;
    my $ready_after = defined $after_state && $after_state == 2 ? 1 : 0;
    my $ready_degraded = $ready_before && !$ready_after ? 1 : 0;
    my $assist_missed = $expected_assist && !$used_assist ? 1 : 0;

    my $report = {
        target_zone                => defined $zone ? $zone : '',
        target_state_before_code   => defined $before_state ? $before_state : -1,
        target_state_after_code    => defined $after_state ? $after_state : -1,
        ready_before               => $ready_before,
        ready_after                => $ready_after,
        ready_degraded             => $ready_degraded,
        expected_assist            => $expected_assist,
        used_assist                => $used_assist,
        assist_missed              => $assist_missed,
        short_label                => index_report_short_label($mode, $used_assist,
                                                               $ready_before,
                                                               $ready_after),
    };

    if ($mode eq 'wildcard_prefix') {
        $report->{wildcard_prefix_hits_delta} = $wildcard_hits;
        add_probe_diagnostic_summary($report, $probe_report);
        add_sqlite_diagnostic_summary($report, $nginx_log);
    }

    if ($mode eq 'exact_fanout') {
        $report->{exact_fanout_delta} = $exact_fanout;
        add_sqlite_diagnostic_summary($report, $nginx_log);
    }

    return $report;
}

sub add_sqlite_diagnostic_summary {
    my ($report, $nginx_log) = @_;

    $report->{sqlite_prepare_failed} = 0;
    $report->{sqlite_locked} = 0;
    $report->{sqlite_no_table} = 0;

    return unless defined $nginx_log && ref($nginx_log->{error_summary}) eq 'HASH';

    $report->{sqlite_prepare_failed} = $nginx_log->{error_summary}->{sqlite_prepare_failed} || 0;
    $report->{sqlite_locked} = $nginx_log->{error_summary}->{sqlite_locked} || 0;
    $report->{sqlite_no_table} = $nginx_log->{error_summary}->{sqlite_no_table} || 0;
}

sub add_probe_diagnostic_summary {
    my ($report, $probe_report) = @_;

    $report->{wildcard_preflight_attempted} = 0;
    $report->{wildcard_preflight_succeeded} = 0;
    $report->{wildcard_preflight_attempts} = 0;
    $report->{wildcard_preflight_probe_key_count} = 0;
    $report->{wildcard_preflight_observed_hits} = 0;

    return unless defined $probe_report && ref($probe_report) eq 'HASH';

    $report->{wildcard_preflight_attempted} = $probe_report->{attempted} ? 1 : 0;
    $report->{wildcard_preflight_succeeded} = $probe_report->{succeeded} ? 1 : 0;
    $report->{wildcard_preflight_attempts} = $probe_report->{attempts} || 0;
    $report->{wildcard_preflight_probe_key_count} = $probe_report->{probe_key_count} || 0;
    $report->{wildcard_preflight_observed_hits} = $probe_report->{observed_hits} || 0;
}

sub zone_index_state_code {
    my ($stats, $zone_name) = @_;
    my $zones;
    my $zone;
    my $index;
    my $state;

    return undef unless defined $stats && ref($stats) eq 'HASH';
    return undef unless defined $zone_name && length $zone_name;

    $zones = $stats->{zones};
    return undef unless ref($zones) eq 'HASH';

    $zone = $zones->{$zone_name};
    return undef unless ref($zone) eq 'HASH';

    $index = $zone->{index};
    return undef unless ref($index) eq 'HASH';

    $state = $index->{state_code};
    return undef unless defined $state && !ref($state)
                        && $state =~ /^-?(?:\d+(?:\.\d+)?|\.\d+)$/;

    return 0 + $state;
}

sub key_index_counter_delta {
    my ($metrics_delta, $counter_name) = @_;
    my $key_index;
    my $value;

    return 0 unless defined $metrics_delta && ref($metrics_delta) eq 'HASH';

    $key_index = $metrics_delta->{key_index};
    return 0 unless ref($key_index) eq 'HASH';

    $value = $key_index->{$counter_name};
    return 0 unless defined $value && !ref($value)
                    && $value =~ /^-?(?:\d+(?:\.\d+)?|\.\d+)$/;

    return 0 + $value;
}

sub index_plan_short_label {
    my ($mode) = @_;

    return 'off' if $mode eq 'disabled';
    return 'ready' if $mode eq 'readiness_only';
    return 'fanout' if $mode eq 'exact_fanout';
    return 'w-prefix' if $mode eq 'wildcard_prefix';

    return 'other';
}

sub index_report_short_label {
    my ($mode, $used_assist, $ready_before, $ready_after) = @_;

    return 'off' if $mode eq 'disabled';

    if ($mode eq 'wildcard_prefix' || $mode eq 'exact_fanout') {
        return 'assist' if $used_assist;
        return 'miss-rdy' if $ready_before;
        return 'miss';
    }

    return 'ready' if $ready_after;
    return 'cfg';
}

sub scenario_ready_requests {
    my ($scenario) = @_;
    my $base_url = "http://127.0.0.1:$options{port}$scenario->{prefix}0";

    if (defined $scenario->{vary_values}
            && ref($scenario->{vary_values}) eq 'ARRAY'
            && @{ $scenario->{vary_values} }
            && defined $scenario->{vary_header}
            && length $scenario->{vary_header}) {
        return map {
            {
                url => $base_url,
                headers => [ $scenario->{vary_header} => $_ ],
            }
        } @{ $scenario->{vary_values} };
    }

    return ({
        url => $base_url,
        headers => [],
    });
}

sub fork_exec {
    my @command = @_;
    my $pid = fork();
    die "fork failed: $!\n" unless defined $pid;

    if ($pid == 0) {
        setpgid(0, 0) or die "setpgid failed: $!\n";
        exec @command or die "exec(@command): $!\n";
    }

    push @active_child_pids, $pid;
    return $pid;
}

sub wait_for_child {
    my ($pid, $label) = @_;
    waitpid($pid, 0);
    @active_child_pids = grep { $_ != $pid } @active_child_pids;
    die "$label failed\n" if $? != 0;
}

sub read_json {
    my ($file) = @_;

    open my $fh, '<', $file or die "open($file): $!";
    local $/;
    my $json = <$fh>;
    close $fh or die "close($file): $!";

    return JSON::PP::decode_json($json);
}

sub update_latest_symlink {
    my ($out_dir, $timestamp) = @_;

    my $latest = "$out_dir/latest";
    unlink $latest if -l $latest || -e $latest;
    symlink($timestamp, $latest) or die "symlink($latest): $!";
}

sub record_nginx_error_log {
    my ($run_dir, $label) = @_;

    my $chunk = $pending_nginx_error_log;
    $pending_nginx_error_log = '';
    $chunk .= consume_nginx_error_log();

    my $safe_label = sanitize_config_name($label);
    my $scenario_log = "$run_dir/${safe_label}_nginx_error.log";
    ensure_empty_file($scenario_log);
    ensure_empty_file("$run_dir/nginx_error.log");

    if (length $chunk) {
        append_text_file($scenario_log, $chunk);
        append_text_file("$run_dir/nginx_error.log", $chunk);
    }

    my $line_count = scalar grep { length $_ } split /\n/, $chunk;
    my $error_summary = summarize_nginx_error_chunk($chunk);

    if (length $chunk) {
        log_info("nginx emitted error-log output for $label");
        print_nginx_error_log($label, $chunk);
    }

    return {
        file       => $scenario_log,
        line_count => $line_count,
        error_summary => $error_summary,
    };
}

sub summarize_nginx_error_chunk {
    my ($chunk) = @_;

    my $summary = {
        sqlite_prepare_failed => 0,
        sqlite_locked => 0,
        sqlite_no_table => 0,
    };

    for my $line (split /\n/, $chunk) {
        if ($line =~ /sqlite prepare failed/) {
            $summary->{sqlite_prepare_failed}++;
        }

        if ($line =~ /database is locked/) {
            $summary->{sqlite_locked}++;
        }

        if ($line =~ /no such table/) {
            $summary->{sqlite_no_table}++;
        }
    }

    return $summary;
}

sub consume_nginx_error_log {
    return '' unless -e $nginx_error_log;

    my $size = -s $nginx_error_log;
    return '' unless defined $size;

    if ($nginx_error_log_offset > $size) {
        $nginx_error_log_offset = 0;
    }

    open my $fh, '<', $nginx_error_log or die "open($nginx_error_log): $!";
    seek($fh, $nginx_error_log_offset, 0) or die "seek($nginx_error_log): $!";
    local $/;
    my $chunk = <$fh>;
    $chunk = '' unless defined $chunk;
    $nginx_error_log_offset = tell($fh);
    close $fh or die "close($nginx_error_log): $!";

    return $chunk;
}

sub stash_nginx_error_log_chunk {
    my $chunk = consume_nginx_error_log();

    if (length $chunk) {
        $pending_nginx_error_log .= $chunk;
    }

    return $chunk;
}

sub append_text_file {
    my ($file, $text) = @_;

    open my $fh, '>>', $file or die "open($file): $!";
    print {$fh} $text or die "write($file): $!";
    close $fh or die "close($file): $!";
}

sub ensure_empty_file {
    my ($file) = @_;

    open my $fh, '>>', $file or die "open($file): $!";
    close $fh or die "close($file): $!";
}

sub print_nginx_error_log {
    my ($label, $chunk) = @_;

    print "\nnginx error log ($label)\n";
    print $chunk;
    print "\n" unless $chunk =~ /\n\z/;
}

sub print_summary_json {
    my ($summary) = @_;

    print "\nSummary JSON\n";
    print JSON::PP->new->ascii->canonical->pretty->encode($summary);
}

sub log_info {
    my ($message) = @_;
    my $stamp = strftime('%H:%M:%S', localtime());
    print "[$stamp] $message\n";
}

sub handle_signal {
    my ($signal_name) = @_;

    return if $shutting_down;
    $shutting_down = 1;
    $shutdown_signal = $signal_name;

    log_info("Received SIG$signal_name, shutting down benchmark");
    cleanup_active_children();

    die "Interrupted by SIG$signal_name\n";
}

sub cleanup_active_children {
    return unless @active_child_pids;

    my @pids = @active_child_pids;
    @active_child_pids = ();

    log_info('Stopping active benchmark worker processes');

    for my $pid (@pids) {
        kill 'TERM', -$pid;
    }

    sleep(0.2);

    for my $pid (@pids) {
        kill 'KILL', -$pid;
        waitpid($pid, 0);
    }
}

sub print_assertion_result {
    my ($assertions) = @_;

    print "\nAssertions\n";

    if ($assertions->{passed}) {
        print "PASS: $assertions->{file}\n";
        return;
    }

    print "FAIL: $assertions->{file}\n";
    for my $violation (@{ $assertions->{violations} }) {
        print "- $violation\n";
    }
}

sub start_redis {
    stop_redis() if -e $redis_pid_file;

    make_path($redis_runtime_dir);

    run_system(
        'redis-server',
        '--port', $redis_port,
        '--daemonize', 'yes',
        '--save', '',
        '--loglevel', 'warning',
        '--pidfile', $redis_pid_file,
        '--logfile', $redis_log_file,
    );

    for (1 .. 50) {
        my $status = system('redis-cli', '-p', $redis_port, 'PING');
        if ($status == 0) {
            run_system('redis-cli', '-p', $redis_port, '-n', $redis_db, 'FLUSHDB');
            return;
        }
        sleep(0.2);
    }

    die "redis-server did not become ready on port $redis_port\n";
}

sub stop_redis {
    return unless -e $redis_pid_file;

    system('redis-cli', '-p', $redis_port, 'shutdown', 'nosave');
    sleep(0.2);
    unlink $redis_pid_file if -e $redis_pid_file;
}

sub run_system {
    my @command = @_;
    system(@command) == 0 or die "command failed: @command\n";
}

sub evaluate_assertions {
    my ($summary, $assert_file) = @_;

    open my $fh, '<', $assert_file or die "open($assert_file): $!";
    local $/;
    my $json = <$fh>;
    close $fh or die "close($assert_file): $!";

    my $assertions = JSON::PP::decode_json($json);
    die "assertion file must be a JSON object\n"
        unless ref($assertions) eq 'HASH';

    my %results_by_scenario = map { $_->{scenario} => $_ } @{ $summary->{scenarios} };
    my @violations;

    my $defaults = $assertions->{defaults};
    if (defined $defaults) {
        die "assertions.defaults must be a JSON object\n"
            unless ref($defaults) eq 'HASH';
    }

    my $scenario_assertions = $assertions->{scenarios};
    if (defined $scenario_assertions) {
        die "assertions.scenarios must be a JSON object\n"
            unless ref($scenario_assertions) eq 'HASH';
    } else {
        $scenario_assertions = {};
    }

    for my $scenario_key (sort keys %results_by_scenario) {
        my $result = $results_by_scenario{$scenario_key};
        my @threshold_sets;

        push @threshold_sets, $defaults if defined $defaults;
        push @threshold_sets, $scenario_assertions->{$scenario_key}
            if exists $scenario_assertions->{$scenario_key};

        for my $thresholds (@threshold_sets) {
            next unless defined $thresholds;
            die "assertions for $scenario_key must be a JSON object\n"
                unless ref($thresholds) eq 'HASH';

            for my $metric_path (sort keys %{$thresholds}) {
                my $rule = $thresholds->{$metric_path};
                die "assertion rule for $scenario_key/$metric_path must be a JSON object\n"
                    unless ref($rule) eq 'HASH';

                my $actual = resolve_metric_path($result, $metric_path);
                push @violations, evaluate_metric_rule(
                    $scenario_key,
                    $metric_path,
                    $actual,
                    $rule,
                );
            }
        }
    }

    return grep { defined $_ } @violations;
}

sub resolve_metric_path {
    my ($data, $path) = @_;

    my $cursor = $data;
    for my $segment (split /\./, $path) {
        die "metric path '$path' is invalid\n"
            unless ref($cursor) eq 'HASH' && exists $cursor->{$segment};
        $cursor = $cursor->{$segment};
    }

    die "metric path '$path' did not resolve to a scalar value\n"
        if ref($cursor);

    die "metric path '$path' did not resolve to a numeric value\n"
        unless defined $cursor && $cursor =~ /^-?(?:\d+(?:\.\d+)?|\.\d+)$/;

    return 0 + $cursor;
}

sub evaluate_metric_rule {
    my ($scenario_key, $metric_path, $actual, $rule) = @_;

    my @allowed = qw(min max);
    for my $key (sort keys %{$rule}) {
        die "unsupported assertion operator '$key' for $scenario_key/$metric_path\n"
            unless grep { $_ eq $key } @allowed;
        die "assertion operator '$key' for $scenario_key/$metric_path must be numeric\n"
            unless defined $rule->{$key}
                && !ref($rule->{$key})
                && $rule->{$key} =~ /^-?(?:\d+(?:\.\d+)?|\.\d+)$/;
    }

    if (exists $rule->{min} && $actual < $rule->{min}) {
        return sprintf('%s %s %.4f < min %.4f',
            $scenario_key,
            $metric_path,
            $actual,
            $rule->{min},
        );
    }

    if (exists $rule->{max} && $actual > $rule->{max}) {
        return sprintf('%s %s %.4f > max %.4f',
            $scenario_key,
            $metric_path,
            $actual,
            $rule->{max},
        );
    }

    return undef;
}

sub discover_config_templates {
    my ($bench_dir) = @_;
    my %templates;

    for my $path (glob("$bench_dir/*.conf")) {
        my ($name) = $path =~ m{/([^/]+)\.conf$};
        next unless defined $name;

        $templates{$name} = {
            name => $name,
            path => $path,
        };
    }

    die "no benchmark config templates found under $bench_dir\n"
        unless %templates;

    return %templates;
}

sub resolve_config_template {
    my ($templates, $value) = @_;

    if (exists $templates->{$value}) {
        return $templates->{$value};
    }

    if (-f $value) {
        my ($name) = $value =~ m{/([^/]+)\.conf$};
        $name = defined $name ? $name : $value;
        return {
            name => $name,
            path => $value,
        };
    }

    die "unknown config template $value\n";
}

sub sanitize_config_name {
    my ($value) = @_;
    $value =~ s/[^A-Za-z0-9_-]+/_/g;
    return $value;
}