use strict;
use warnings;

@ARGV == 2 or die "usage: perl t/pvs_targets.pl module-config nginx-makefile\n";
sub read_file {
    open my $file, '<', $_[0] or die "$_[0]: $!";
    local $/;
    return <$file>;
}

my ($sources) = read_file($ARGV[0]) =~ /CACHE_PURGE_SRCS="([^"]+)"/s;
die "missing module source manifest\n" unless defined $sources;
$sources =~ s/\\\r?\n/ /g;
my @paths = split ' ', $sources;
die "empty module source manifest\n" unless @paths;
my @units = map {
    m{^\$ngx_addon_dir/(\w+)\.c$} or die "unsupported module source: $_\n";
    $1;
} @paths;
my $makefile = read_file($ARGV[1]);
my @targets;
for my $unit (@units) {
    my @matches = $makefile =~ m{^(objs/addon/[^\s:]+/\Q$unit\E\.o):}mg;
    die "expected one object target for $unit, found " . scalar(@matches) . "\n"
        unless @matches == 1;
    push @targets, $matches[0];
}
print join("\n", @targets), "\n";
