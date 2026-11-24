#!/bin/sh
# Starts dnsmasq, which answers for dyn.test (the "resolve" tests) from
# /tmp/dns/hosts and for the SRV record _http._tcp.srv.test, then socat,
# which answers on 127.0.0.1:9301 with /tmp/raw/resp verbatim, then nginx.
# test.sh rewrites those files (and sends dnsmasq SIGHUP).
set -e

mkdir -p /tmp/dns /tmp/hc /tmp/raw
printf '127.0.0.1 dyn.test\n127.0.0.2 dyn.test\n' > /tmp/dns/hosts
printf 'HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n' > /tmp/raw/resp

dnsmasq --keep-in-foreground --no-resolv --no-hosts \
        --addn-hosts=/tmp/dns/hosts \
        --srv-host=_http._tcp.srv.test,a.srv.test,9201 \
        --host-record=a.srv.test,127.0.0.4 \
        --listen-address=127.0.0.1 --port=5353 --bind-interfaces &

socat TCP-LISTEN:9301,bind=127.0.0.1,reuseaddr,fork EXEC:/raw.sh &

exec nginx -g 'daemon off;'
