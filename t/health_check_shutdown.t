#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check when nginx shuts down with a check in progress:
# the worker closes it, rather than waiting for it or leaving its socket
# open.

###############################################################################

use warnings;
use strict;

use Test::More;

use IO::Select;
use IO::Socket::INET;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http proxy upstream_zone/)->plan(2);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # 127.0.0.1:8093 accepts connections and never answers: a check is
    # always in progress

    upstream stall {
        zone stall 256k;
        server 127.0.0.1:8093;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location / {
            proxy_pass http://stall;
            health_check interval=1s uri=/health;
        }
    }
}

EOF

$t->run_daemon(\&silent_daemon, port(8093));
$t->waitforsocket('127.0.0.1:' . port(8093))
	or die "Can't start silent daemon";

$t->run();

###############################################################################

select undef, undef, undef, 2;

my $start = time();
$t->stop();

cmp_ok(time() - $start, '<', 10, 'exits within 10s');
unlike($t->read_file('error.log'), qr/open socket/, 'no socket left open');

###############################################################################

# Accepts connections, takes what is sent, and never answers.

sub silent_daemon {
	my ($port) = @_;

	my $server = IO::Socket::INET->new(
		Proto => 'tcp',
		LocalAddr => "127.0.0.1:$port",
		Listen => 128,
		Reuse => 1
	)
		or die "Can't create listening socket: $!\n";

	my $sel = IO::Select->new($server);
	my $buf;

	while (my @ready = $sel->can_read) {
		for my $fh (@ready) {
			if ($fh == $server) {
				$sel->add($server->accept());

			} elsif (!sysread($fh, $buf, 4096)) {
				$sel->remove($fh);
				close $fh;
			}
		}
	}
}

###############################################################################
