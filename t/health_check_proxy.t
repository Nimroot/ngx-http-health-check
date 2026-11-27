#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check with the location's proxy settings: the HTTP
# version, proxy_bind, and the timeouts, with a server that never answers.

###############################################################################

use warnings;
use strict;

use Test::More;

use IO::Select;
use IO::Socket::INET;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;
use Test::Nginx::HealthCheck;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http proxy rewrite upstream_zone/)
	->plan(5);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # The backend answers "/" with its port; /health is 503 with
    # hc/<port>.fail, and logs the checks of hc_slow, the only ones that
    # get there. /health11 is /health for HTTP/1.1 and 426 for
    # HTTP/1.0, as Envoy does. /from-bind is 200 only for a client at
    # 127.0.0.2. 127.0.0.1:8093 accepts connections and never answers.

    server {
        listen       127.0.0.1:8081;
        server_name  localhost;

        default_type text/plain;

        location = /health {
            access_log %%TESTDIR%%/health.log;
            if (-f %%TESTDIR%%/hc/$server_port.fail) { return 503; }
            return 200 "ok\n";
        }

        location = /health11 {
            if ($server_protocol != "HTTP/1.1") { return 426; }
            rewrite ^ /health last;
        }

        location = /from-bind {
            if ($remote_addr != 127.0.0.2) { return 403; }
            return 200 "ok\n";
        }

        location / {
            return 200 "$server_port\n";
        }
    }

    upstream hc_h10 {
        zone hc_h10 256k;
        server 127.0.0.1:8081;
    }

    upstream hc_bind {
        zone hc_bind 256k;
        server 127.0.0.1:8081;
    }

    upstream hc_slow {
        zone hc_slow 256k;
        server 127.0.0.1:8093;
        server 127.0.0.1:8081;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location /h10/ {
            proxy_pass http://hc_h10/;
            proxy_http_version 1.0;
            health_check interval=1s uri=/health11;
        }

        location /bind/ {
            proxy_pass http://hc_bind/;
            proxy_bind 127.0.0.2;
            health_check interval=1s uri=/from-bind;
        }

        # Not requested: 127.0.0.1:8093 would hold the request. The
        # timeouts differ so that its check says which one ran out.
        location /slow/ {
            proxy_pass http://hc_slow/;
            proxy_connect_timeout 1s;
            proxy_send_timeout 2s;
            proxy_read_timeout 6s;
            health_check interval=1s uri=/health;
        }
    }
}

EOF

set_state($t);

$t->run_daemon(\&silent_daemon, port(8093));
$t->waitforsocket('127.0.0.1:' . port(8093))
	or die "Can't start silent daemon";

$t->run();

###############################################################################

my $p1 = port(8081);
my $mark;

logged($t, 0, qq{peer 127.0.0.1:$p1 in upstream "hc_h10" is unhealthy: }
	. 'status 426 is not 2xx or 3xx',
	'HTTP/1.0 check gets 426 where HTTP/1.1 is needed');
expect('/bind/', $p1, "checks come from proxy_bind's address");

# hc_slow: each server is checked on its own schedule, 127.0.0.1:8081 every
# second while the checks of 127.0.0.1:8093 wait for proxy_read_timeout
# (6s); in 5s, 8081 gets about 5 checks, not 1 at most

my $checks = lines($t, 'health.log');
select undef, undef, undef, 5;
cmp_ok(lines($t, 'health.log') - $checks, '>=', 3,
	'server checked every second next to one that never answers');

$mark = log_mark($t);
set_state($t, "$p1.fail");
logged($t, $mark, qq{peer 127.0.0.1:$p1 in upstream "hc_slow" is unhealthy},
	'its state changes meanwhile');

logged($t, 0, 'peer 127.0.0.1:' . port(8093) . ' in upstream "hc_slow" '
	. 'is unhealthy: timed out reading the response',
	'server that never answers fails on proxy_read_timeout');

###############################################################################

sub lines {
	my ($t, $name) = @_;

	return 0 unless -e $t->testdir() . '/' . $name;
	return scalar(() = $t->read_file($name) =~ /\n/g);
}

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
