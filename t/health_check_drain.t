#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check with a draining server: only clients bound to it
# by "sticky" get it. It is checked like any other, and when unhealthy it
# loses them too.

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

my $t = Test::Nginx->new()->has(qw/http proxy upstream_zone upstream_sticky/)
	->plan(6);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;
worker_processes 4;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # The backends answer "/" with their port; /health is 503 with
    # hc/<port>.fail.

    server {
        listen       127.0.0.1:8081;
        listen       127.0.0.1:8082;
        server_name  localhost;

        default_type text/plain;

        location = /health {
            if (-f %%TESTDIR%%/hc/$server_port.fail) { return 503; }
            return 200 "ok\n";
        }

        location / {
            return 200 "$server_port\n";
        }
    }

    upstream hc_drain {
        zone hc_drain 256k;
        sticky cookie srv;
        server 127.0.0.1:8081 route=a drain;
        server 127.0.0.1:8082 route=b;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location / {
            proxy_pass http://hc_drain;
            health_check interval=1s uri=/health;
        }
    }
}

EOF

set_state($t);
$t->run();

###############################################################################

my ($p1, $p2) = (port(8081), port(8082));
my %bound = (headers => "Cookie: srv=a\n");
my $mark;

expect('/', $p2, 'new clients get the other server');
expect('/', $p1, 'bound clients still get the draining server', %bound);

$mark = log_mark($t);
set_state($t, "$p1.fail");
expect('/', $p2, 'unhealthy, it loses its bound clients', %bound);
logged($t, $mark, qq{peer 127.0.0.1:$p1 in upstream "hc_drain" is unhealthy},
	'the draining server is checked');

set_state($t);
expect('/', $p1, 'healthy again, it gets them back', %bound);
now('/', $p2, 'still draining for new clients');

###############################################################################
