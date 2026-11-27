#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check of https servers: SNI, verification of the
# server's certificate, and failures for a certificate for another name or
# from an untrusted issuer.

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

my $t = Test::Nginx->new()->has(qw/http http_ssl proxy rewrite upstream_zone/)
	->has_daemon('openssl')->plan(7);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # 127.0.0.1:8083 and :8084 answer "/" with their port, as tls.test;
    # 8083 needs SNI tls.test, see the next server. /health is 503 with
    # hc/<port>.fail; /health11 is /health for HTTP/1.1 and 426 for
    # HTTP/1.0.

    server {
        listen       127.0.0.1:8083 ssl;
        listen       127.0.0.1:8084 ssl;
        server_name  tls.test;

        ssl_certificate tls.test.crt;
        ssl_certificate_key tls.test.key;

        default_type text/plain;

        location = /health {
            if (-f %%TESTDIR%%/hc/$server_port.fail) { return 503; }
            return 200 "ok\n";
        }

        location = /health11 {
            if ($server_protocol != "HTTP/1.1") { return 426; }
            rewrite ^ /health last;
        }

        location / {
            return 200 "$server_port\n";
        }
    }

    # handshakes on 127.0.0.1:8083 without SNI tls.test
    server {
        listen       127.0.0.1:8083 ssl default_server;
        ssl_reject_handshake on;
    }

    upstream tls.test {             # 8084 if 8083 is unhealthy
        zone hc_tls 256k;
        server 127.0.0.1:8083 max_fails=0;
        server 127.0.0.1:8084 backup;
    }

    upstream wrong.test {           # certificate for another name
        zone hc_tls_name 256k;
        server 127.0.0.1:8084;
    }

    upstream hc_tls_ca {            # certificate from an untrusted issuer
        zone hc_tls_ca 256k;
        server 127.0.0.1:8084;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location /tls/ {
            proxy_pass https://tls.test/;
            proxy_http_version 1.1;     # the default from nginx 1.29.7
            proxy_ssl_server_name on;
            proxy_ssl_verify on;
            proxy_ssl_trusted_certificate tls.test.crt;
            health_check interval=1s uri=/health11;
        }

        location /tls-name/ {
            proxy_pass https://wrong.test/;
            proxy_ssl_verify on;
            proxy_ssl_trusted_certificate tls.test.crt;
            health_check interval=1s uri=/health;
        }

        location /tls-ca/ {
            proxy_pass https://hc_tls_ca/;
            proxy_ssl_name tls.test;
            proxy_ssl_verify on;
            proxy_ssl_trusted_certificate other.test.crt;
            health_check interval=1s uri=/health;
        }
    }
}

EOF

my $d = $t->testdir();

foreach my $name ('tls.test', 'other.test') {
	system('openssl req -x509 -new -newkey ec '
		. '-pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 '
		. "-subj /CN=$name -addext subjectAltName=DNS:$name "
		. "-out $d/$name.crt -keyout $d/$name.key "
		. ">>$d/openssl.out 2>&1") == 0
		or die "Can't create certificate for $name: $!\n";
}

set_state($t);
$t->run();

###############################################################################

my ($p3, $p4) = (port(8083), port(8084));
my $mark;

expect('/tls/', $p3, 'SNI and verification pass');

$mark = log_mark($t);
set_state($t, "$p3.fail");
expect('/tls/', $p4, 'unhealthy server taken out, backup used');
logged($t, $mark, qq{peer 127.0.0.1:$p3 in upstream "tls.test" is unhealthy: }
	. 'status 503 is not 2xx or 3xx', 'reason is the status');

$mark = log_mark($t);
set_state($t);
expect('/tls/', $p3, 'server back');
logged($t, $mark, qq{peer 127.0.0.1:$p3 in upstream "tls.test" is healthy},
	'healthy again');

logged($t, 0, qq{peer 127.0.0.1:$p4 in upstream "wrong.test" is unhealthy: }
	. 'SSL certificate does not match "wrong.test"', 'name mismatch fails');
logged($t, 0, qq{peer 127.0.0.1:$p4 in upstream "hc_tls_ca" is unhealthy: }
	. 'SSL certificate verify error', 'untrusted issuer fails');

###############################################################################
