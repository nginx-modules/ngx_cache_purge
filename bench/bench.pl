#!/usr/bin/env perl

use strict;
use warnings;

use File::Basename qw(dirname);
use File::Path qw(make_path remove_tree);
use FindBin;
use Getopt::Long qw(GetOptions);
use JSON::PP ();
use LWP::UserAgent;
use POSIX qw(setpgid strftime);
use Time::HiRes qw(sleep);

use lib "$FindBin::Bin/lib";
use Bench qw(fetch_stats format_table stats_delta write_json);

my %options = (
    count       => 1000,
    concurrency => 50,
    out_dir     => "$FindBin::Bin/results",
    port        => 18080,
);

GetOptions(
    'assert-file=s' => \$options{assert_file},
    'count=i'       => \$options{count},
    'config-template=s' => \$options{config_template},
    'concurrency=i' => \$options{concurrency},
    'out-dir=s'     => \$options{out_dir},
    'port=i'        => \$options{port},
    'quick'         => \$options{quick},
    'scenarios=s'   => \$options{scenarios},
) or die "invalid arguments\n";

die "--count must be > 0\n" unless $options{count} > 0;
die "--concurrency must be > 0\n" unless $options{concurrency} > 0;

my $duration = $options{quick} ? 15 : 60;
my $perl = $^X;
my $nginx = '/opt/nginx/sbin/nginx';
my $nginx_error_log = '/tmp/bench_nginx_error.log';
my $nginx_prefix = '/tmp/bench_nginx';
my $get_worker = "$FindBin::Bin/worker_get.pl";
my $purge_worker = "$FindBin::Bin/worker_purge.pl";
my $redis_pid_file = '/tmp/bench_redis.pid';
my $redis_port = 16379;
my $redis_db = 5;
my $stats_url = "http://127.0.0.1:$options{port}/bench_stats";
my $ua = LWP::UserAgent->new(agent => 'ngx-cache-pilot-bench/1.0', keep_alive => 5, timeout => 5);

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
    },
    {
        key        => 'wild',
        name       => 'wildcard_purge',
        table_name => 'wildcard_purge',
        config_template => 'nginx',
        prefix     => '/wild/',
        mode       => 'wildcard',
        backend    => 'sqlite',
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
    },
);

my %scenario_by_key = map { $_->{key} => $_ } @all_scenarios;
my @selected_keys = defined $options{scenarios}
    ? split(/,/, $options{scenarios})
    : map { $_->{key} } @all_scenarios;

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

$SIG{INT} = sub { handle_signal('INT'); };
$SIG{TERM} = sub { handle_signal('TERM'); };

END {
    cleanup_active_children();
    eval { stop_nginx($current_conf) if defined $current_conf; };
    eval { stop_redis() if $redis_started; };
}

prepare_output_dir($options{out_dir});
log_info('Preparing benchmark runtime state');
cleanup_runtime_state();

my $timestamp = strftime('%Y%m%d-%H%M%S', localtime());
my $run_dir = "$options{out_dir}/$timestamp";
make_path($run_dir);
log_info("Writing run artifacts to $run_dir");

my @results;
my $active_runtime;

for my $scenario (@scenarios) {
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
    duration_s   => $duration,
    count        => $options{count},
    concurrency  => $options{concurrency},
    port         => $options{port},
    scenarios    => \@results,
};

if (defined $options{assert_file}) {
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

sub run_scenario {
    my ($scenario, $duration_s, $run_dir, $stats_endpoint) = @_;

    log_info("Fetching baseline stats for $scenario->{name}");
    my $before = fetch_stats($stats_endpoint);
    log_info("Warming cache for $scenario->{name}");
    warm_cache($scenario->{prefix}, $options{count});

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

sub cleanup_runtime_state {
    for my $path (
        '/tmp/bench_cache_exact',
        '/tmp/bench_cache_wild',
        '/tmp/bench_cache_tag_sqlite',
        '/tmp/bench_cache_tag_redis',
        '/tmp/bench_proxy_temp',
        '/tmp/bench_client_body_temp',
        $nginx_prefix,
    ) {
        remove_tree($path, { error => \my $error });
        if ($error && @{$error}) {
            for my $entry (@{$error}) {
                my ($failed_path, $message) = %{$entry};
                die "remove_tree($failed_path): $message\n" if defined $message;
            }
        }
    }

    for my $file (
        '/tmp/bench_tags.db',
        '/tmp/bench_tags.db-shm',
        '/tmp/bench_tags.db-wal',
        '/tmp/bench_nginx.pid',
        $nginx_error_log,
        '/tmp/bench_redis.log',
        $redis_pid_file,
    ) {
        unlink $file if -e $file;
    }

    $nginx_error_log_offset = 0;

    unlink $_ for glob('/tmp/bench_nginx_*.conf');

    make_path(
        '/tmp/bench_proxy_temp',
        '/tmp/bench_client_body_temp',
        $nginx_prefix,
        "$nginx_prefix/logs",
    );
    ensure_nginx_prefix();
}

sub render_runtime_config {
    my ($scenario) = @_;

    my $backend = $scenario->{backend};
    my $template_name = sanitize_config_name($scenario->{config_template_name});
    my $target = "/tmp/bench_nginx_${template_name}_${backend}.conf";

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
    return unless -e '/tmp/bench_nginx.pid';

    ensure_nginx_prefix();
    system($nginx, '-p', $nginx_prefix, '-c', $conf, '-s', 'stop');
    sleep(0.2);
    unlink '/tmp/bench_nginx.pid' if -e '/tmp/bench_nginx.pid';
}

sub ensure_nginx_prefix {
    make_path($nginx_prefix, "$nginx_prefix/logs");

    my $prefix_error_log = "$nginx_prefix/logs/error.log";
    if (!-e $prefix_error_log) {
        open my $prefix_fh, '>', $prefix_error_log or die "open($prefix_error_log): $!";
        close $prefix_fh or die "close($prefix_error_log): $!";
    }

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
        sleep(0.2);
    }

    die "nginx did not become ready at $url\n";
}

sub warm_cache {
    my ($prefix, $count) = @_;

    for my $index (0 .. ($count - 1)) {
        my $response = $ua->get("http://127.0.0.1:$options{port}$prefix$index");
        die "warm-up failed for $prefix$index: " . $response->status_line . "\n"
            unless $response->is_success;
    }

    sleep(0.5);
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

    my $chunk = consume_nginx_error_log();
    return undef unless length $chunk;

    my $safe_label = sanitize_config_name($label);
    my $scenario_log = "$run_dir/${safe_label}_nginx_error.log";
    append_text_file($scenario_log, $chunk);
    append_text_file("$run_dir/nginx_error.log", $chunk);

    my $line_count = scalar grep { length $_ } split /\n/, $chunk;

    log_info("nginx emitted error-log output for $label");
    print_nginx_error_log($label, $chunk);

    return {
        file       => $scenario_log,
        line_count => $line_count,
    };
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

sub append_text_file {
    my ($file, $text) = @_;

    open my $fh, '>>', $file or die "open($file): $!";
    print {$fh} $text or die "write($file): $!";
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

    run_system(
        'redis-server',
        '--port', $redis_port,
        '--daemonize', 'yes',
        '--save', '',
        '--loglevel', 'warning',
        '--pidfile', $redis_pid_file,
        '--logfile', '/tmp/bench_redis.log',
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