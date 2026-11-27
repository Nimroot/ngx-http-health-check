#!/usr/bin/perl

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Tests for health_check with servers from DNS ("resolve"): servers added
# are checked, removing an unhealthy one leaves the count of usable servers
# correct, and an SRV server down by configuration gets neither checks nor
# requests.

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

my $t = Test::Nginx->new()->has(qw/http proxy rewrite upstream_zone/)
	->plan(17);

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;
worker_processes 4;

events {
}

http {
    %%TEST_GLOBALS_HTTP%%

    # dyn.test resolves to what the file "dns" lists, among 127.0.0.1-3,
    # whose backends answer "/" with their address; /health is 503 with
    # hc/<address>.fail. The SRV record _http._tcp.srv.test points to
    # a.srv.test (127.0.0.4), whose backend logs the checks it gets.
    # 127.0.0.1:8082 answers with its port. /flaky always answers 503, to
    # count proxy_next_upstream attempts.

    server {
        listen       127.0.0.1:%%PORT_8091%%;
        listen       127.0.0.2:%%PORT_8091%%;
        listen       127.0.0.3:%%PORT_8091%%;
        listen       127.0.0.1:8082;
        server_name  localhost;

        default_type text/plain;

        location = /health {
            if (-f %%TESTDIR%%/hc/$server_addr.fail) { return 503; }
            return 200 "ok\n";
        }

        location = /flaky {
            return 503;
        }

        location / {
            if ($server_port = %%PORT_8082%%) { return 200 "$server_port\n"; }
            return 200 "$server_addr\n";
        }
    }

    server {
        listen       127.0.0.4:%%PORT_8092%%;
        server_name  localhost;

        default_type text/plain;

        location = /health {
            access_log %%TESTDIR%%/srv-health.log;
            return 200 "ok\n";
        }

        location / {
            return 200 "127.0.0.4\n";
        }
    }

    resolver 127.0.0.1:%%PORT_8981_UDP%% valid=1s ipv6=off;

    # /flaky/ would trip the passive checks (max_fails) and take servers
    # out on their own.

    upstream hc_dns {
        zone hc_dns 256k;
        server dyn.test:%%PORT_8091%% resolve max_fails=0;
    }

    upstream hc_srv {
        zone hc_srv 256k;
        server 127.0.0.1:8082;
        server srv.test service=http resolve down;
    }

    upstream hc_srv_up {            # no health_check: shows the SRV resolves
        zone hc_srv_up 256k;
        server srv.test service=http resolve;
    }

    server {
        listen       127.0.0.1:8080;
        server_name  localhost;

        location /dns/ {
            proxy_pass http://hc_dns/;
            health_check interval=1s uri=/health;
        }

        # One attempt per server the group counts as usable (peers->tries).
        location /flaky/ {
            proxy_pass http://hc_dns/flaky;
            proxy_next_upstream error timeout http_503;
            add_header X-Upstream-Status $upstream_status always;
        }

        location /srv/ {
            proxy_pass http://hc_srv/;
            health_check interval=1s uri=/health;
        }

        location /srv-up/ {
            proxy_pass http://hc_srv_up/;
        }
    }
}

EOF

set_state($t);
dns($t, '127.0.0.1', '127.0.0.2');

$t->run_daemon(\&dns_daemon, $t, port(8981), port(8092))
	->waitforfile($t->testdir() . '/' . port(8981));

$t->run();

###############################################################################

my $mark;

expect('/dns/', peers('127.0.0.1', '127.0.0.2'), 'both addresses');
is(attempts(), '503 503, 503', 'flaky: two attempts');

# 127.0.0.2 fails

$mark = log_mark($t);
set_state($t, '127.0.0.2.fail');
expect('/dns/', '127.0.0.1', 'failing address taken out');
logged($t, $mark, 'peer 127.0.0.2:' . port(8091)
	. ' in upstream "hc_dns" is unhealthy', 'warn for the resolved server');
is(attempts(), '503 503', 'flaky: unhealthy server not counted');

# DNS drops the unhealthy 127.0.0.2

dns($t, '127.0.0.1');
select undef, undef, undef, 3;
now('/dns/', '127.0.0.1', 'dropped address');
is(attempts(), '503 503', 'flaky: still one attempt');

# DNS adds 127.0.0.3

dns($t, '127.0.0.1', '127.0.0.3');
expect('/dns/', peers('127.0.0.1', '127.0.0.3'), 'added address used');
is(attempts(), '503 503, 503', 'flaky: two attempts again');

# DNS adds 127.0.0.2 back, still failing

$mark = log_mark($t);
dns($t, '127.0.0.1', '127.0.0.2', '127.0.0.3');
logged($t, $mark, 'peer 127.0.0.2:' . port(8091)
	. ' in upstream "hc_dns" is unhealthy',
	'address added back is checked and taken out');
expect('/dns/', peers('127.0.0.1', '127.0.0.3'), 'healthy addresses only');
is(attempts(), '503 503, 503', 'flaky: two attempts still');

# 127.0.0.2 recovers

set_state($t);
expect('/dns/', peers('127.0.0.1', '127.0.0.2', '127.0.0.3'),
	'all three addresses');
is(attempts(), '503 503, 503, 503', 'flaky: three attempts');

# srv.test service=http is a.srv.test, 127.0.0.4: down by configuration

expect('/srv-up/', '127.0.0.4', 'the SRV record resolves');
select undef, undef, undef, 3;
now('/srv/', port(8082), 'SRV server down by configuration stays down');
is(lines($t, 'srv-health.log'), 0, 'SRV server down by configuration '
	. 'not checked');

###############################################################################

# the status of a /flaky/ request and its $upstream_status, e.g.
# "503 503, 503": two attempts, both answered 503

sub attempts {
	my $r = http_get('/flaky/') || '';

	my ($status) = $r =~ m!^HTTP/1\.\d (\d{3})!;
	my ($upstream) = $r =~ /^X-Upstream-Status: (.*?)\x0d?$/mi;

	return ($status || '') . ' ' . ($upstream || '');
}

sub lines {
	my ($t, $name) = @_;

	return 0 unless -e $t->testdir() . '/' . $name;
	return scalar(() = $t->read_file($name) =~ /\n/g);
}

# dns($t, @addresses): dyn.test resolves to these addresses from now on

sub dns {
	my ($t, @addresses) = @_;

	my $d = $t->testdir();

	$t->write_file('dns.new', join(' ', @addresses));
	rename "$d/dns.new", "$d/dns" or die "Can't rename dns.new: $!\n";
}

###############################################################################

# A DNS server for dyn.test (A records from the file "dns"), the SRV record
# _http._tcp.srv.test (a.srv.test, at $srv_port), and a.srv.test (A record
# 127.0.0.4). Other names don't exist.

sub dns_daemon {
	my ($t, $port, $srv_port) = @_;

	my $socket = IO::Socket::INET->new(
		LocalAddr => '127.0.0.1',
		LocalPort => $port,
		Proto => 'udp',
	)
		or die "Can't create listening socket: $!\n";

	# signal we are ready

	open my $fh, '>', $t->testdir() . '/' . $port;
	close $fh;

	while (1) {
		my $query;
		$socket->recv($query, 65536) or next;

		my $reply = dns_reply($t, $query, $srv_port);
		$socket->send($reply) if defined $reply;
	}
}

sub dns_reply {
	my ($t, $query, $srv_port) = @_;

	use constant A		=> 1;
	use constant SRV	=> 33;
	use constant IN		=> 1;
	use constant TTL	=> 1;

	return undef if length $query < 12;

	# the question: its name, type and class

	my ($offset, @labels) = (12);

	while ($offset < length $query) {
		my $len = unpack("\@$offset C", $query);
		last if $len == 0;
		push @labels, substr($query, $offset + 1, $len);
		$offset += $len + 1;
	}

	my ($type) = unpack("\@" . ($offset + 1) . " n", $query);
	my $question = substr($query, 12, $offset + 5 - 12);
	my $name = lc join('.', @labels);

	# the answers, each naming the question's name (pointer 0xc00c)

	my ($rcode, @answers) = (0);

	if ($name eq 'dyn.test') {
		if ($type == A) {
			push @answers, pack('n3N n C4', 0xc00c, A, IN, TTL, 4,
				split(/\./, $_))
				for split(' ', $t->read_file('dns'));
		}

	} elsif ($name eq '_http._tcp.srv.test') {
		if ($type == SRV) {
			my $target = pack('(C/a*)3 x', 'a', 'srv', 'test');
			push @answers, pack('n3N n n3', 0xc00c, SRV, IN, TTL,
				6 + length($target), 1, 1, $srv_port) . $target;
		}

	} elsif ($name eq 'a.srv.test') {
		if ($type == A) {
			push @answers, pack('n3N n C4', 0xc00c, A, IN, TTL, 4,
				127, 0, 0, 4);
		}

	} else {
		$rcode = 3;		# NXDOMAIN
	}

	return pack('n6', unpack('n', $query), 0x8180 | $rcode, 1,
		scalar @answers, 0, 0) . $question . join('', @answers);
}

###############################################################################
