#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for reading a check's response, served byte for byte: framing by
# Content-Length, chunked Transfer-Encoding or the end of the connection,
# interim responses, the header's size, and what makes a response invalid.

###############################################################################

use warnings;
use strict;

use Test::More;

use IO::Socket::INET;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;
use Test::Nginx::HealthCheck;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http proxy upstream_zone/)->plan(42);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # 127.0.0.1:8090 answers every request with the bytes of the file
    # "raw", whatever they are, and closes the connection. While it is
    # healthy, requests get its response ("ok", 204, or 502 if nginx's
    # proxy rejects it); once it is unhealthy, the backup server
    # 127.0.0.1:8082 answers with its port. The backup server is checked
    # too, and passes.

    server {
        listen       127.0.0.1:8082;
        server_name  localhost;

        location = /health {
            return 200 "ok\n";
        }

        location / {
            return 200 "$server_port\n";
        }
    }

    upstream hc_raw {
        zone hc_raw 256k;
        server 127.0.0.1:8090 max_fails=0;
        server 127.0.0.1:8082 backup;
    }

    upstream hc_raw11 {
        zone hc_raw11 256k;
        server 127.0.0.1:8090 max_fails=0;
        server 127.0.0.1:8082 backup;
    }

    match hc_raw_ok {               # the decoded body, not the chunks
        status 200;
        body ~ "^ok$";
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location /raw/ {
            proxy_pass http://hc_raw/;
            proxy_http_version 1.0;
            proxy_buffer_size 1k;
            health_check interval=1s uri=/health;
        }

        location /raw11/ {
            proxy_pass http://hc_raw11/;
            proxy_http_version 1.1;     # the default from nginx 1.29.7
            health_check interval=1s uri=/health match=hc_raw_ok;
        }
    }
}

EOF

my $ok = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n";

raw($t, $ok);

$t->run_daemon(\&raw_daemon, $t, port(8090));
$t->waitforsocket('127.0.0.1:' . port(8090)) or die "Can't start raw daemon";

$t->run();

###############################################################################

my $backup = port(8082);
my $mark;

# framing

expect('/raw/', 'ok', 'complete response passes');

$mark = log_mark($t);
raw($t, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nok\n");
expect('/raw/', $backup, 'body shorter than Content-Length fails');
logged($t, $mark, 'peer 127.0.0.1:' . port(8090) . ' in upstream "hc_raw" '
	. 'is unhealthy: connection closed before the response body was '
	. 'complete', 'reason: incomplete body');

raw($t, "HTTP/1.0 200 OK\r\n\r\nok\n");
expect('/raw/', 'ok', 'close-delimited body passes');

$mark = log_mark($t);
raw($t, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 100\r\n\r\n"
	. "ok\n");
expect('/raw/', $backup, 'conflicting Content-Length fails');
logged($t, $mark, 'is unhealthy: duplicate Content-Length',
	'reason: second Content-Length');

raw($t, "HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n");
expect('/raw/', '204', '204 needs no body');

raw($t, "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\n"
	. "ok\n");
expect('/raw/', $backup, 'repeated Content-Length fails, as for the proxy');

raw($t, $ok);
expect('/raw/', 'ok', 'complete response passes again');

raw($t, "HTTP/1.1 200 OK\r\nContent-Length : 100\r\n\r\nok\n");
expect('/raw/', $backup, 'whitespace before colon fails');

# interim responses and the header's size, in hc_raw's proxy_buffer_size 1k

raw($t, "HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\n"
	. $ok);
expect('/raw/', 'ok', '103 Early Hints skipped, 200 passes');

$mark = log_mark($t);
raw($t, "HTTP/1.1 200 OK\r\nX-Pad: " . ('0' x 1500) . "\r\n"
	. "Content-Length: 3\r\n\r\nok\n");
expect('/raw/', $backup, 'header over proxy_buffer_size fails');
logged($t, $mark, 'upstream "hc_raw" is unhealthy: response header too large',
	'reason: header too large');
expect('/raw11/', 'ok', 'fits a larger proxy_buffer_size');

raw($t, "HTTP/1.1 103 Early Hints\r\nLink: <" . ('0' x 680) . ">; "
	. "rel=preload\r\n\r\nHTTP/1.1 200 OK\r\nX-Pad: " . ('0' x 400)
	. "\r\nContent-Length: 3\r\n\r\nok\n");
expect('/raw/', 'ok', 'each header gets the whole proxy_buffer_size');

$mark = log_mark($t);
raw($t, early(17) . $ok);
expect('/raw/', $backup, '17 interim responses fail');
logged($t, $mark, 'upstream "hc_raw" is unhealthy: too many interim responses',
	'reason: too many interim responses');

# before nginx 1.29.8, the proxy takes a second 103 in a row for the final
# response and returns it to the client: the log tells the check's result

$mark = log_mark($t);
raw($t, early(16) . $ok);
logged($t, $mark, 'peer 127.0.0.1:' . port(8090) . ' in upstream "hc_raw" '
	. 'is healthy', '16 interim responses pass');

$mark = log_mark($t);
raw($t, early(1));
expect('/raw/', $backup, 'closed after an interim response fails');
logged($t, $mark, 'upstream "hc_raw" is unhealthy: connection closed before '
	. 'the response header was complete', 'reason: header incomplete');

raw($t, $ok);
expect('/raw/', 'ok', 'complete response passes once more');

# no response, or not HTTP

$mark = log_mark($t);
raw($t, '');
expect('/raw/', $backup, 'closed without a response fails');
logged($t, $mark, 'is unhealthy: connection closed without a response',
	'reason: no response');

raw($t, $ok);
expect('/raw/', 'ok', 'complete response passes yet again');

$mark = log_mark($t);
raw($t, "\x15\x03\x03\x00\x02\x02\x46");
expect('/raw/', $backup, 'TLS alert fails');
logged($t, $mark, 'is unhealthy: TLS data instead of an HTTP response',
	'reason: TLS instead of HTTP');

raw($t, $ok);
expect('/raw/', 'ok', 'healthy at the end');

# chunked bodies; hc_raw11 has match body ~ "^ok$", which the chunks would
# fail

raw($t, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	. "1\r\no\r\n2\r\nk\n\r\n0\r\n\r\n");
expect('/raw11/', 'ok', 'chunked: complete response passes');

$mark = log_mark($t);
raw($t, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nok\n\r\n");
expect('/raw11/', $backup, 'chunked: no last chunk fails');
logged($t, $mark, 'peer 127.0.0.1:' . port(8090) . ' in upstream "hc_raw11" '
	. 'is unhealthy: connection closed before the response body was '
	. 'complete', 'reason: incomplete chunked body');

raw($t, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	. "1\r\no\r\n2\r\nk\n\r\n0\r\n\r\n");
expect('/raw11/', 'ok', 'chunked: chunks decoded, healthy again');
expect('/raw/', 'ok', 'chunked for HTTP/1.0 decoded, as by the proxy');

$mark = log_mark($t);
raw($t, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
	. "Content-Length: 3\r\n\r\n3\r\nok\n\r\n0\r\n\r\n");
expect('/raw11/', $backup, 'chunked: with Content-Length fails');
logged($t, $mark, 'is unhealthy: both Content-Length and Transfer-Encoding',
	'reason: both lengths');

raw($t, "HTTP/1.1 200 OK\r\nTRANSFER-ENCODING: Chunked\r\n\r\n"
	. "3;a=b\r\nok\n\r\n0\r\nX-Trailer: 1\r\n\r\n");
expect('/raw11/', 'ok', 'chunked: extensions, trailers: healthy');

$mark = log_mark($t);
raw($t, "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
	. "0\r\n\r\n");
expect('/raw11/', $backup, 'chunked: other transfer coding fails');
logged($t, $mark, 'upstream "hc_raw11" is unhealthy: unsupported '
	. 'Transfer-Encoding', 'reason: unsupported coding');

raw($t, $ok);
expect('/raw11/', 'ok', 'chunked: plain response, healthy');

$mark = log_mark($t);
raw($t, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
	. "zz\r\nok\n\r\n0\r\n\r\n");
expect('/raw11/', $backup, 'chunked: invalid chunk size fails');
logged($t, $mark, 'upstream "hc_raw11" is unhealthy: invalid chunked body',
	'reason: invalid chunked body');

raw($t, $ok);
expect('/raw11/', 'ok', 'chunked: healthy at the end');
expect('/raw/', 'ok', 'healthy at the end too');

###############################################################################

# raw($t, $bytes): the raw daemon's response from now on

sub raw {
	my ($t, $bytes) = @_;

	my $d = $t->testdir();

	$t->write_file('raw.new', $bytes);
	rename "$d/raw.new", "$d/raw" or die "Can't rename raw.new: $!\n";
}

# early($n): $n "103 Early Hints" responses

sub early {
	my ($n) = @_;

	return "HTTP/1.1 103 Early Hints\r\n\r\n" x $n;
}

# Reads a request's header, answers with the file "raw" and closes the
# connection.

sub raw_daemon {
	my ($t, $port) = @_;

	my $server = IO::Socket::INET->new(
		Proto => 'tcp',
		LocalAddr => "127.0.0.1:$port",
		Listen => 128,
		Reuse => 1
	)
		or die "Can't create listening socket: $!\n";

	local $SIG{PIPE} = 'IGNORE';

	while (my $client = $server->accept()) {
		$client->autoflush(1);

		while (<$client>) {
			last if /^\x0d?\x0a?$/;
		}

		print $client $t->read_file('raw');
		close $client;
	}
}

###############################################################################
