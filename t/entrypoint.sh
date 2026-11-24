#!/bin/sh
# Starts dnsmasq, which answers for dyn.test (the "resolve" tests) from
# /tmp/dns/hosts, then nginx. test.sh rewrites that file and sends SIGHUP.
set -e

mkdir -p /tmp/dns /tmp/hc
printf '127.0.0.1 dyn.test\n127.0.0.2 dyn.test\n' > /tmp/dns/hosts

dnsmasq --keep-in-foreground --no-resolv --no-hosts \
        --addn-hosts=/tmp/dns/hosts \
        --listen-address=127.0.0.1 --port=5353 --bind-interfaces &

exec nginx -g 'daemon off;'
