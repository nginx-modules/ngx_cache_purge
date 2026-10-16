use strict;
use warnings;

# Minimal local origins for real NGINX cache-fill tests. The wire formats match
# ngx_http_{fastcgi,scgi,uwsgi}_module.c; these are fixtures, not app servers.
sub gateway_read {
    my ($socket, $size) = @_;
    my $data = '';
    while (length($data) < $size) {
        my $n = sysread($socket, my $part, $size - length($data));
        die 'short gateway request' unless $n;
        $data .= $part;
    }
    return $data;
}

sub gateway_record {
    my ($type, $id, $body) = @_;
    return pack('CCnnCC', 1, $type, $id, length($body), 0, 0) . $body;
}

sub gateway_respond {
    my ($socket, $protocol) = @_;
    my $id = 1;
    if ($protocol eq 'fastcgi') {
        while (1) {
            my ($version, $type, $request, $length, $padding) =
                unpack('CCnnCC', gateway_read($socket, 8));
            die 'invalid FastCGI request' unless $version == 1 && $request;
            $id = $request;
            gateway_read($socket, $length + $padding);
            last if $type == 5 && $length == 0; # end of STDIN
        }
    } elsif ($protocol eq 'scgi') {
        my $length = '';
        while (1) {
            my $c = gateway_read($socket, 1);
            last if $c eq ':';
            die 'invalid SCGI netstring' unless $c =~ /[0-9]/ && length($length) < 8;
            $length .= $c;
        }
        my %params = split /\0/, gateway_read($socket, $length);
        die 'invalid SCGI parameters' unless $params{SCGI} eq '1'
            && $params{CONTENT_LENGTH} eq '0' && $params{REQUEST_METHOD} eq 'GET';
        die 'missing SCGI terminator' unless gateway_read($socket, 1) eq ',';
    } elsif ($protocol eq 'uwsgi') {
        my (undef, $length) = unpack('CvC', gateway_read($socket, 4));
        gateway_read($socket, $length);
    } else {
        die "unknown gateway protocol: $protocol";
    }
    my $body = "$protocol origin content\n";
    my $response = "Status: 200 OK\r\nContent-Type: text/plain\r\n"
        . "ETag: \"gateway-v1\"\r\nCache-Control: max-age=3600\r\nContent-Length: " . length($body)
        . "\r\n\r\n$body";
    print {$socket} $protocol eq 'fastcgi'
        ? gateway_record(6, $id, $response) . gateway_record(6, $id, '')
          . gateway_record(3, $id, pack('NCxxx', 0, 0))
        : $response;
}

1;
