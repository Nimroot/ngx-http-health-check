#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check with more servers than free connections: checks
# that don't fit wait for a connection, and fail no server.

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

my $t = Test::Nginx->new()->has(qw/http proxy upstream_zone/)->plan(2);

# 40 checks every second (the same server, 40 times), a quarter of 16
# worker_connections at a time

my $servers = join '', map { "        server 127.0.0.1:8081;\n" } 1 .. 40;

$t->write_file_expand('nginx.conf', <<"EOF");

%%TEST_GLOBALS%%

daemon off;

events {
    worker_connections 16;
}

http {
    %%TEST_GLOBALS_HTTP%%

    upstream many {
        zone many 256k;
$servers    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location / {
            proxy_pass http://many;
            health_check interval=1s uri=/health;
        }
    }
}

EOF

# the server, a daemon of its own: nginx's connections are for the checks

$t->run_daemon(\&http_daemon, port(8081));
$t->waitforsocket('127.0.0.1:' . port(8081)) or die "Can't start daemon";

$t->run();

###############################################################################

select undef, undef, undef, 4;

is(served('/'), 'ok', 'server still in use');
is(scalar(() = $t->read_file('error.log') =~ /is unhealthy/g), 0,
	'no check failed for lack of connections');

###############################################################################

# Answers every request with 200 "ok".

sub http_daemon {
	my ($port) = @_;

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

		print $client "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
			. "Connection: close\r\n\r\nok\n";
		close $client;
	}
}

###############################################################################
