#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check and match configuration: "nginx -t" accepts valid
# configurations and rejects each unsupported or invalid one with a clear
# message.

###############################################################################

use warnings;
use strict;

use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }

use lib 'lib';
use Test::Nginx;

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

my $t = Test::Nginx->new()->has(qw/http proxy upstream_zone/)->plan(25);

###############################################################################

my $ok = qr/test is successful/;

like(conf($t, srv('location / { proxy_pass http://z; '
	. 'health_check interval=10s uri=/hc match=m; }')), $ok,
	'all three parameters');
like(conf($t, srv('location / { proxy_pass http://z; health_check; }')), $ok,
	'bare health_check');
like(conf($t, srv(locations(48))), $ok, '48 checks of one group');
like(conf($t, srv(locations(49))),
	failed('too many health checks for upstream "z"'),
	'49 checks of one group');
like(conf($t, srv('location / { proxy_pass http://nz; health_check; }')),
	failed('must reside in shared memory'), 'upstream without zone');
like(conf($t, srv('location / { proxy_pass http://z; '
	. 'health_check match=nope; }')),
	failed('match "nope" not found'), 'unknown match');
like(conf($t, srv('location / { proxy_pass http://$host; health_check; }')),
	failed('needs "proxy_pass" to an upstream group'),
	'proxy_pass with a variable');
like(conf($t, srv('location / { health_check; }')),
	failed('needs "proxy_pass" to an upstream group'), 'no proxy_pass');

# https

like(conf($t, srv('location / { proxy_pass https://z; health_check; }')),
	$ok, 'https upstream');
like(conf($t, srv('location / { proxy_pass https://z; '
	. 'proxy_ssl_name $host; health_check; }')),
	failed('with variables'), 'https: proxy_ssl_name with a variable');
like(conf($t, srv('location / { proxy_pass https://z; '
	. 'proxy_ssl_certificate /c/$host.crt; '
	. 'proxy_ssl_certificate_key /c/$host.key; health_check; }')),
	failed('with variables'), 'https: certificate with a variable');
like(conf($t, srv('location / { proxy_pass http://z; '
	. 'proxy_bind $remote_addr; health_check; }')),
	$ok, 'proxy_bind with a variable');

# parameters

like(conf($t, srv('location / { proxy_pass http://z; health_check fails=3; }')),
	failed('"fails=3" is not supported'), 'unsupported parameter fails=');
like(conf($t, srv('location / { proxy_pass http://z; '
	. 'health_check mandatory; }')),
	failed('"mandatory" is not supported'), 'unsupported parameter mandatory');
like(conf($t, srv('location / { proxy_pass http://z; '
	. 'health_check interval=soon; }')),
	failed('invalid parameter "interval=soon"'), 'invalid interval');
like(conf($t, srv('location / { proxy_pass http://z; '
	. 'health_check "uri=/a b"; }')),
	failed('invalid parameter "uri=/a b"'), 'uri with a space');
like(conf($t, srv('location / { proxy_pass http://z; health_check uri=hc; }')),
	failed('invalid parameter "uri=hc"'), 'uri without a slash');
like(conf($t, srv('health_check;')),
	failed('"health_check" directive is not allowed here'),
	'health_check outside location');

# match blocks

like(conf($t, <<'EOF'), $ok, 'match: every form of test');
    match all {
        status ! 500;
        status 200 204;
        status ! 301 302;
        status 200-399;
        status ! 400-599;
        status 301-303 307;
        header Content-Type = application/json;
        header Content-Type != application/json;
        header Cache-Control ~ no-store;
        header Cache-Control !~ no-store;
        header Server;
        header ! X-Debug;
        body ~ "ready";
        body !~ "starting up";
    }
EOF
like(conf($t, 'match r { require $host; }'),
	failed('"require" in "match" is not supported'),
	'match: require not supported');
like(conf($t, 'match r { status 600; }'), failed('invalid status "600"'),
	'match: invalid status');
like(conf($t, 'match r { header X == y; }'), failed('invalid operator "=="'),
	'match: invalid operator');
like(conf($t, 'match r { body = y; }'), failed('invalid operator "="'),
	'match: no = for the body');
like(conf($t, 'match r { body ~ "("; }'), failed('pcre2_compile() failed'),
	'match: invalid regex');
like(conf($t, 'match m { status 200; }'), failed('duplicate match "m"'),
	'match: duplicate name');

###############################################################################

# conf($t, $http): what "nginx -t" says of a configuration with $http in its
# http block, next to an upstream group with a zone (z), one without (nz),
# and a match block (m)

sub conf {
	my ($t, $http) = @_;

	$t->write_file_expand('test.conf', <<"EOF");

%%TEST_GLOBALS%%

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    upstream z {
        zone z 256k;
        server 127.0.0.1:1;
    }

    upstream nz {
        server 127.0.0.1:1;
    }

    match m {
        status 200;
    }

$http
}

EOF

	my $d = $t->testdir();

	return `$Test::Nginx::NGINX -t -p $d/ -c test.conf -e error.log 2>&1`;
}

# srv($body): a server block with $body

sub srv {
	my ($body) = @_;

	return "server { listen 127.0.0.1:8080; $body }";
}

# locations($n): $n locations, each with a check of z

sub locations {
	my ($n) = @_;

	return join ' ', map { "location /$_ { proxy_pass http://z; "
		. 'health_check; }' } 1 .. $n;
}

# failed($message): what "nginx -t" says when it fails with $message

sub failed {
	my ($message) = @_;

	return qr/\Q$message\E.*test failed/s;
}

###############################################################################
