use strict;
use warnings;
use JSON::PP qw(encode_json);

# Pin one representative per supported API boundary, not a version cross product.
my $full = ($ENV{CI_FULL} || '') eq 'true';
my $run = 1;
my $base = $ENV{CI_BASE} || '';
if (!$full && $base =~ /^[0-9a-f]{40}$/ && $base !~ /^0+$/) {
    if (open my $git, '-|', 'git', 'diff', '--name-only', $base, 'HEAD') {
        my @changed = <$git>;
        if (close $git && @changed) {
            chomp @changed;
            $run = scalar grep {
                !m{^(?:README(?:\.[A-Za-z-]+)?\.md|docs/.*|LICENSE|COPYING|\.github/(?:ISSUE_TEMPLATE|PULL_REQUEST_TEMPLATE)/.*)$}
            } @changed;
        }
    }
}

my @integration = (
    {name => 'stable', nginx => '1.30.6', ubuntu => '24.04'},
    {name => 'mainline', nginx => '1.31.7', ubuntu => '24.04'},
);
my @compatibility = (
    {name => 'minimum', nginx => '1.7.9', cc => 'gcc', modes => 'incremental'},
    {name => 'epoll fallback', nginx => '1.7.11', cc => 'gcc', modes => 'pooled'},
    {name => 'first safe thread pools', nginx => '1.7.12', cc => 'gcc', modes => 'pooled'},
    {name => 'GCC + Clang', nginx => '1.31.7', cc => 'gcc clang', modes => 'incremental pooled'},
);
if ($full) {
    push @integration,
        {name => 'legacy TLS', nginx => '1.20.2', ubuntu => '20.04'},
        {name => 'legacy stable', nginx => '1.26.3', ubuntu => '24.04'},
        {name => 'previous stable', nginx => '1.28.3', ubuntu => '24.04'};
    push @compatibility,
        {name => 'manager seconds', nginx => '1.11.4', cc => 'gcc', modes => 'incremental'},
        {name => 'manager milliseconds', nginx => '1.11.5', cc => 'gcc', modes => 'pooled'};
}
open my $out, '>>', $ENV{GITHUB_OUTPUT} or die "GITHUB_OUTPUT: $!";
print {$out} 'run=', ($run ? 'true' : 'false'), "\n",
             'full=', ($full ? 'true' : 'false'), "\n",
             'integration=', encode_json({include => \@integration}), "\n",
             'compatibility=', encode_json({include => \@compatibility}), "\n";
close $out or die $!;
open my $summary, '>>', $ENV{GITHUB_STEP_SUMMARY} or die "GITHUB_STEP_SUMMARY: $!";
print {$summary} "### CI scope\n\n",
    $run ? ($full ? "Extended checks.\n\n" : "Commit checks.\n\n")
         : "Documentation-only change: compilation and runtime jobs skipped.\n\n";
print {$summary} "| Group | NGINX versions |\n|---|---|\n",
    '| Integration | ', join(', ', map { $_->{nginx} } @integration), " |\n",
    '| Compatibility | ', join(', ', map { $_->{nginx} } @compatibility), " |\n";
close $summary or die $!;
