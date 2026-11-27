package Test::Nginx::HealthCheck;

# Copyright (C) 2026 The ngx_http_health_check authors
# SPDX-License-Identifier: BSD-2-Clause

# Helpers for the module's tests, on top of nginx-tests' Test::Nginx:
# which backends answer a location, waiting for a server's state to
# change, a backend's health switched by files, and the error log from
# a point on.

###############################################################################

use warnings;
use strict;

use base qw/ Exporter /;

our @EXPORT = qw/ served peers expect now set_state log_mark logged
	not_logged /;

use Test::More qw//;
use Test::Nginx;

###############################################################################

# seconds to wait for a state change (checks run every second)

our $WAIT = $ENV{TEST_NGINX_HC_WAIT} || 8;

# served($uri, %extra): who answers 8 requests to $uri, as peers() puts
# it: the body of a 200 response (a backend answers with its key), the
# status otherwise; $extra{headers}: more request header lines

sub served {
	my ($uri, %extra) = @_;
	my %got;

	my $headers = $extra{headers} || '';

	for (1 .. 8) {
		my $r = http(<<EOF);
GET $uri HTTP/1.0
Host: localhost
$headers
EOF

		if (!defined $r
			|| $r !~ m!^HTTP/1\.\d (\d{3}).*?\x0d\x0a\x0d\x0a(.*)!s)
		{
			$got{'none'} = 1;
			next;
		}

		my ($status, $body) = ($1, $2);
		$body =~ s/\n\z//;

		$got{$status eq '200' ? $body : $status} = 1;
	}

	return peers(keys %got);
}

# peers(@keys): the keys as served() returns them, e.g. "8081,8082"

sub peers {
	return join ',', sort @_;
}

# expect($uri, $want, $name, %extra): waits up to $WAIT seconds for $uri
# to be served by $want

sub expect {
	my ($uri, $want, $name, %extra) = @_;
	my $got;

	my $start = time();

	while (($got = served($uri, %extra)) ne $want) {
		last if time() - $start >= $WAIT;
		select undef, undef, undef, 0.5;
	}

	local $Test::Builder::Level = $Test::Builder::Level + 1;
	Test::More::is($got, $want, $name);
}

# now($uri, $want, $name, %extra): checked once, a state that must hold

sub now {
	my ($uri, $want, $name, %extra) = @_;

	local $Test::Builder::Level = $Test::Builder::Level + 1;
	Test::More::is(served($uri, %extra), $want, $name);
}

# set_state($t, @files): the backends' state, as files in hc/ that the
# backends' configuration tests, e.g. "8081.fail"; none: all healthy

sub set_state {
	my ($t, @files) = @_;

	my $dir = $t->testdir() . '/hc';

	mkdir $dir;
	unlink glob("$dir/*");

	$t->write_file("hc/$_", '') for @files;
}

# log_mark($t): where the error log ends now, for logged() and not_logged()

sub log_mark {
	my ($t) = @_;

	return length $t->read_file('error.log');
}

# logged($t, $mark, $text, $name): $text appears in the error log after
# $mark within $WAIT seconds

sub logged {
	my ($t, $mark, $text, $name) = @_;
	my $found;

	my $start = time();

	until ($found = index(substr($t->read_file('error.log'), $mark), $text)
		>= 0)
	{
		last if time() - $start >= $WAIT;
		select undef, undef, undef, 0.25;
	}

	local $Test::Builder::Level = $Test::Builder::Level + 1;
	Test::More::ok($found, $name) or Test::More::diag("not logged: $text");
}

# not_logged($t, $mark, $text, $name): $text doesn't appear in the error log
# after $mark within 2 seconds

sub not_logged {
	my ($t, $mark, $text, $name) = @_;

	select undef, undef, undef, 2;

	local $Test::Builder::Level = $Test::Builder::Level + 1;
	Test::More::ok(index(substr($t->read_file('error.log'), $mark), $text)
		< 0, $name) or Test::More::diag("logged: $text");
}

###############################################################################

1;

###############################################################################
