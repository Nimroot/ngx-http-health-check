#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for two health checks of one group: a server is used only while it
# passes both. Its state lives in shared memory, so new workers start from
# it.

###############################################################################

use warnings;
use strict;

use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;
use Test::Nginx::HealthCheck;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http proxy upstream_zone/);

plan(skip_all => 'no /proc, to find the workers') unless -e '/proc/self/stat';

$t->plan(9);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;
worker_processes 4;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # The backends answer "/" with their port; /health is 503 with
    # hc/<port>.fail, /ready with hc/<port>.unready.

    server {
        listen       127.0.0.1:8081;
        listen       127.0.0.1:8082;
        server_name  localhost;

        default_type text/plain;

        location = /health {
            if (-f %%TESTDIR%%/hc/$server_port.fail) { return 503; }
            return 200 "ok\n";
        }

        location = /ready {
            if (-f %%TESTDIR%%/hc/$server_port.unready) { return 503; }
            return 200 "ok\n";
        }

        location / {
            return 200 "$server_port\n";
        }
    }

    upstream hc_two {
        zone hc_two 256k;
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location /two/ {
            proxy_pass http://hc_two/;
            health_check interval=1s uri=/health;
        }

        location /two-ready/ {
            proxy_pass http://hc_two/;
            health_check interval=1s uri=/ready;
        }
    }
}

EOF

my ($p1, $p2) = (port(8081), port(8082));

set_state($t, "$p1.fail", "$p1.unready");
$t->run();

###############################################################################

my $d = $t->testdir();
my $mark;

# the first server fails both checks, then passes /health only

expect('/two/', $p2, 'first server taken out');
$mark = log_mark($t);
set_state($t, "$p1.unready");
logged($t, $mark, qq{peer 127.0.0.1:$p1 in upstream "hc_two" }
	. "passes health_check in $d/nginx.conf:",
	'notice for the check that passes');
now('/two/', $p2, 'still out, fails /ready');

# the workers restart while it fails /ready

$mark = log_mark($t);
kill 'TERM', workers($t);
logged($t, $mark, 'start worker process', 'restart: new workers started');
now('/two/', $p2, 'restart: stays out');
select undef, undef, undef, 1;
now('/two/', $p2, 'restart: still out');
not_logged($t, $mark, qq{peer 127.0.0.1:$p1 in upstream "hc_two" is unhealthy},
	'restart: state kept, no new warning');

# it passes both

$mark = log_mark($t);
set_state($t);
expect('/two/', peers($p1, $p2), 'back once it passes both');
logged($t, $mark, qq{peer 127.0.0.1:$p1 in upstream "hc_two" is healthy},
	'healthy');

###############################################################################

# the pids of nginx's worker processes: the children of its master

sub workers {
	my ($t) = @_;

	my $master = $t->read_file('nginx.pid');
	chomp $master;

	my @pids;

	for my $stat (glob '/proc/[0-9]*/stat') {
		open my $fh, '<', $stat or next;
		my ($pid, $ppid) = (<$fh> // '') =~ /^(\d+) \(.*\) \S+ (\d+)/;
		close $fh;

		push @pids, $pid if defined $ppid && $ppid == $master;
	}

	return @pids;
}

###############################################################################
