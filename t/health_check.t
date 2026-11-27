#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check: servers taken out and put back as their checks
# fail and pass, match tests, servers down by configuration, the count of
# usable servers, and what is logged.

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

my $t = Test::Nginx->new()->has(qw/http proxy rewrite upstream_zone/)
	->plan(34);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;
worker_processes 4;             # checks run in worker 0, the others must see them

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # The backends answer "/" with their port. /health is 503 "fail" with
    # hc/<port>.fail, 200 "degraded" with hc/<port>.degraded, 200 "ok" as
    # text/html with hc/<port>.html, and 200 "ok" as text/plain otherwise.
    # /flaky always answers 503, to count proxy_next_upstream attempts.

    server {
        listen       127.0.0.1:8081;
        listen       127.0.0.1:8082;
        server_name  localhost;

        default_type text/plain;

        location = /health {
            if (-f %%TESTDIR%%/hc/$server_port.fail)     { return 503 "fail\n"; }
            if (-f %%TESTDIR%%/hc/$server_port.degraded) { return 200 "degraded\n"; }
            if (-f %%TESTDIR%%/hc/$server_port.html)     { rewrite ^ /health-html last; }
            return 200 "ok\n";
        }

        location = /health-html {
            internal;
            default_type text/html;
            return 200 "ok\n";
        }

        location = /flaky {
            return 503 "$server_port\n";
        }

        location / {
            return 200 "$server_port\n";
        }
    }

    # hc_default also serves /flaky/, whose 503s would trip the passive
    # checks (max_fails) and take servers out on their own.

    upstream hc_default {
        zone hc_default 256k;
        server 127.0.0.1:8081 max_fails=0;
        server 127.0.0.1:8082 max_fails=0;
    }

    upstream hc_match {
        zone hc_match 256k;
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
    }

    upstream hc_neg {
        zone hc_neg 256k;
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
    }

    upstream hc_root {
        zone hc_root 256k;
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
    }

    upstream hc_admin {
        zone hc_admin 256k;
        server 127.0.0.1:8081 down;
        server 127.0.0.1:8082;
    }

    upstream hc_unchecked {
        zone hc_unchecked 256k;
        server 127.0.0.1:8081;
        server 127.0.0.1:8082;
    }

    match hc_ok {
        status 200;
        header Content-Type = text/plain;
        body ~ "^ok";
    }

    match hc_neg {                  # negative tests only
        status ! 500-599;
        header ! X-Maintenance;
        header Content-Type != text/html;
        body !~ "degraded";
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location /default/ {
            proxy_pass http://hc_default/;
            health_check interval=1s uri=/health;
        }

        location /match/ {
            proxy_pass http://hc_match/;
            health_check interval=1s uri=/health match=hc_ok;
        }

        location /neg/ {
            proxy_pass http://hc_neg/;
            health_check interval=1s uri=/health match=hc_neg;
        }

        location /root/ {
            proxy_pass http://hc_root/;
            health_check interval=1s;
        }

        location /admin/ {
            proxy_pass http://hc_admin/;
            health_check interval=1s uri=/health;
        }

        location /unchecked/ {
            proxy_pass http://hc_unchecked/;
        }

        # One attempt per server the group counts as usable (peers->tries).
        location /flaky/ {
            proxy_pass http://hc_default/flaky;
            proxy_next_upstream error timeout http_503;
            add_header X-Upstream-Status $upstream_status always;
        }
    }
}

EOF

set_state($t);
$t->run();

###############################################################################

my ($p1, $p2) = (port(8081), port(8082));
my $mark;

# all healthy

expect('/default/', peers($p1, $p2), 'default: both servers');
expect('/match/', peers($p1, $p2), 'match: both servers');
expect('/neg/', peers($p1, $p2), 'neg: both servers');
expect('/root/', peers($p1, $p2), 'root: both servers');
now('/admin/', $p2, 'admin: down by configuration');
now('/unchecked/', peers($p1, $p2), 'unchecked: both servers');
is(attempts(), '503 503, 503', 'flaky: one attempt per server');

# the first server's /health fails with 503

$mark = log_mark($t);
set_state($t, "$p1.fail");
expect('/default/', $p2, 'default: first server taken out');
expect('/match/', $p2, 'match: first server taken out');
expect('/neg/', $p2, 'neg: status ! 500-599 takes it out');
now('/root/', peers($p1, $p2), 'root: uri=/ still passes');
now('/unchecked/', peers($p1, $p2), 'unchecked: untouched');
is(attempts(), '503 503', 'flaky: unhealthy server not counted');
logged($t, $mark, qq{peer 127.0.0.1:$p1 in upstream "hc_default" }
	. 'is unhealthy: status 503 is not 2xx or 3xx',
	'warn names peer, upstream, reason');
logged($t, $mark, 'upstream "hc_neg" is unhealthy: '
	. 'status 503 fails "status ! 500-599"',
	'reason names the failed status test');

# it recovers

$mark = log_mark($t);
set_state($t);
expect('/default/', peers($p1, $p2), 'default: back');
expect('/match/', peers($p1, $p2), 'match: back');
now('/admin/', $p2, 'admin: down by configuration stays down');
is(attempts(), '503 503, 503', 'flaky: counted again');
logged($t, $mark, qq{peer 127.0.0.1:$p1 in upstream "hc_default" is healthy},
	'notice when healthy again');

# it answers 200 "degraded"

$mark = log_mark($t);
set_state($t, "$p1.degraded");
expect('/match/', $p2, 'match: body ~ ^ok fails');
expect('/neg/', $p2, 'neg: body !~ degraded fails');
now('/default/', peers($p1, $p2), 'default: 2xx passes without match');
logged($t, $mark, 'response fails "body ~ ^ok"',
	'reason names the failed body test');

# it answers 200 "ok" as text/html

set_state($t);
expect('/match/', peers($p1, $p2), 'match: healthy first');
expect('/neg/', peers($p1, $p2), 'neg: healthy first');
$mark = log_mark($t);
set_state($t, "$p1.html");
expect('/match/', $p2, 'match: header = text/plain fails');
expect('/neg/', $p2, 'neg: header != text/html fails');
now('/default/', peers($p1, $p2), 'default: both servers');
logged($t, $mark, 'response fails "header Content-Type = text/plain"',
	'reason names the failed header test');

# both servers fail, then recover

set_state($t, "$p1.fail", "$p2.fail");
expect('/default/', '502', 'default: no healthy server, 502');
now('/root/', peers($p1, $p2), 'root: unaffected');

set_state($t);
expect('/default/', peers($p1, $p2), 'default: both servers again');
expect('/match/', peers($p1, $p2), 'match: both servers again');

###############################################################################

# the status of a /flaky/ request and its $upstream_status, e.g.
# "503 503, 503": two attempts, both answered 503

sub attempts {
	my $r = http_get('/flaky/') || '';

	my ($status) = $r =~ m!^HTTP/1\.\d (\d{3})!;
	my ($upstream) = $r =~ /^X-Upstream-Status: (.*?)\x0d?$/mi;

	return ($status || '') . ' ' . ($upstream || '');
}

###############################################################################
