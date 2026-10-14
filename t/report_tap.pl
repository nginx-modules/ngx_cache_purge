use strict;
use warnings;

my ($raw, $errors, $report, $status) = @ARGV;
sub read_file {
    open my $fh, '<', $_[0] or die "$_[0]: $!";
    local $/;
    return <$fh> // '';
}
my $text = read_file($raw);
my $valid = $text =~ /(<testsuites\b[^>]*>.*<\/testsuites>)\s*$/s;
my $xml = $valid ? $1 : '';
if (!$valid) {
    # Bailouts may prevent the formatter from writing XML at all. Keep this a
    # failed harness result, and retain the original stdout/stderr as artifacts.
    $text .= "\n" . read_file($errors);
    $text =~ s/[^\x09\x0A\x0D\x20-\x7E]/?/g;
    $text =~ s/\]\]>/]]]]><![CDATA[>/g;
    $xml = '<testsuites><testsuite name="tap_harness" tests="1" failures="0" errors="1">'
         . '<testcase name="TAP execution"><error message="TAP did not produce a complete JUnit report"/></testcase>'
         . '<system-out><![CDATA[' . $text . ']]></system-out></testsuite></testsuites>';
}
open my $fh, '>', $report or die "$report: $!";
print {$fh} "$xml\n" or die $!;
close $fh or die $!;
exit($valid ? 0 : ($status ? 0 : 1));
