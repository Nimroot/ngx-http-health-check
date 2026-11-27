#!/usr/bin/env bash
# Black-box tests (see the Makefile: make test). Runs the test image in a
# container of its own, breaks and repairs backends (files in /tmp/hc/),
# changes DNS answers (/tmp/dns/hosts) and raw responses (/tmp/raw/resp),
# and checks where requests go and what is logged. Then checks configuration
# errors with `nginx -t`. Exit code = number of failures.
set -uo pipefail

ENGINE="${ENGINE:-podman}"
IMAGE="${IMAGE:-localhost/ngx-health-check-test:1.30.6}"
WAIT="${WAIT:-8}"      # seconds to wait for a state change (checks run every 1s)
C="ngx-health-check-test-$$"

pass=0 fail=0
ok()  { printf '  \e[32mPASS\e[0m %-60s %s\n' "$1" "$2"; ((pass++)); }
bad() { printf '  \e[31mFAIL\e[0m %-60s %s\n' "$1" "$2"; ((fail++)); }
is()  { [[ $2 == "$3" ]] && ok "$1" "$2" || bad "$1" "got '$2', want '$3'"; }

# --init: nginx isn't process 1, which would reap (and log) the helpers'
# orphans
"$ENGINE" run -d --init --name "$C" -p 127.0.0.1::8082 "$IMAGE" >/dev/null || exit 1
cleanup() {
    ((fail)) && "$ENGINE" logs "$C" 2>&1 | tail -n 40
    "$ENGINE" rm -f "$C" >/dev/null
}
trap cleanup EXIT
BASE="http://$("$ENGINE" port "$C" 8082/tcp | head -n 1)"

cx() { "$ENGINE" exec "$C" sh -c "$1"; }
set_state() { cx "mkdir -p /tmp/hc && rm -f /tmp/hc/* && ${1:-true}"; }

# dns ADDR...: dyn.test resolves to these addresses from now on
dns() {
    "$ENGINE" exec "$C" sh -c \
        'for a in "$@"; do echo "$a dyn.test"; done > /tmp/dns/hosts && pkill -HUP dnsmasq' \
        sh "$@"
}

# raw FORMAT: socat's response from now on, as a printf format
raw() {
    "$ENGINE" exec "$C" sh -c \
        'printf "$1" > /tmp/raw/resp.new && mv /tmp/raw/resp.new /tmp/raw/resp' \
        sh "$1"
}

# early N: N 103 Early Hints responses, as a printf format for raw
early() {
    local i
    for ((i = 0; i < $1; i++)); do
        printf '%s' 'HTTP/1.1 103 Early Hints\r\n\r\n'
    done
}

# served PATH [CURL_ARG...]: who answers 8 requests, e.g. "9101,9102", "502"
served() {
    local path=$1 i out
    shift
    for i in {1..8}; do
        out=$(curl -s -m 5 -w ' %{http_code}' "$@" "$BASE$path")
        out=${out//$'\n'/}
        if [[ $out == *" 200" ]]; then echo "${out% *}"; else echo "${out##* }"; fi
    done | sort -u | paste -sd, -
}

# expect NAME PATH WANT [CURL_ARG...]: waits up to $WAIT seconds for PATH to
# be served by WANT
expect() {
    local name=$1 path=$2 want=$3 got start=$SECONDS
    shift 3
    while :; do
        got=$(served "$path" "$@")
        [[ $got == "$want" ]] && { ok "$name" "$got ($((SECONDS - start))s)"; return; }
        ((SECONDS - start >= WAIT)) && { bad "$name" "got $got, want $want"; return; }
        sleep 0.5
    done
}

# now NAME PATH WANT [CURL_ARG...]: checked once (a state that must hold)
now() {
    local name=$1 path=$2 want=$3
    shift 3
    is "$name" "$(served "$path" "$@")" "$want"
}

# attempts NAME PATH WANT: status and $upstream_status of a /flaky/ request,
# e.g. "503 503, 503" = two attempts, both answered 503
attempts() {
    is "$1" "$(curl -s -m 5 -o /dev/null -w '%{http_code} %header{x-upstream-status}' "$BASE$2")" "$3"
}

# logged NAME TEXT: TEXT appears in the log since $since (polled: the engine
# can take a moment to collect the container's output)
logged() {
    local i
    for i in {1..12}; do
        "$ENGINE" logs --since "$since" "$C" 2>&1 | grep -F -- "$2" >/dev/null \
            && { ok "$1" "logged"; return; }
        sleep 0.25
    done
    bad "$1" "not in log: $2"
}

# not_logged NAME TEXT: TEXT doesn't appear in the log since $since
not_logged() {
    sleep 2
    "$ENGINE" logs --since "$since" "$C" 2>&1 | grep -F -- "$2" >/dev/null \
        && bad "$1" "in log: $2" || ok "$1" "not logged"
}

# extra_nginx DIR: starts a second nginx in the container, with its
# config, pid and log in DIR and the http block read from stdin
extra_nginx() {
    local dir=$1 http
    http=$(cat)
    "$ENGINE" exec -i "$C" sh -c "mkdir -p $dir && cat > $dir/nginx.conf && nginx -c $dir/nginx.conf" <<EOF
load_module modules/ngx_http_health_check_module.so;
worker_processes 1;
error_log $dir/error.log info;
pid $dir/nginx.pid;
events { worker_connections 16; }
http {
    access_log off;
$http
}
EOF
}

for _ in {1..40}; do curl -sf -m 1 "$BASE/healthz" >/dev/null && break; sleep 0.25; done

echo "all healthy"
set_state
expect "default: both servers"                 /default/   9101,9102
expect "match: both servers"                   /match/     9101,9102
expect "neg: both servers"                     /neg/       9101,9102
expect "root: both servers"                    /root/      9101,9102
now    "admin: 9101 is down by config"         /admin/     9102
now    "unchecked: both servers"               /unchecked/ 9101,9102
attempts "flaky: one attempt per server"       /flaky/default/ "503 503, 503"

echo "9101 /health -> 503"
since=$(date +%s); set_state "touch /tmp/hc/9101.fail"
expect "default: 9101 taken out"               /default/   9102
expect "match: 9101 taken out"                 /match/     9102
expect "neg: status ! 500-599 takes 9101 out"  /neg/       9102
now    "root: uri=/ still passes, both"        /root/      9101,9102
now    "unchecked: untouched, both"            /unchecked/ 9101,9102
attempts "flaky: unhealthy server not counted" /flaky/default/ "503 503"
logged "warn names peer, upstream, reason" \
       'peer 127.0.0.1:9101 in upstream "hc_default" is unhealthy: status 503 is not 2xx or 3xx'
logged "reason names the failed status test" \
       'upstream "hc_neg" is unhealthy: status 503 fails "status ! 500-599"'

echo "9101 recovers"
since=$(date +%s); set_state
expect "default: 9101 back"                    /default/   9101,9102
expect "match: 9101 back"                      /match/     9101,9102
now    "admin: config-down 9101 stays down"    /admin/     9102
attempts "flaky: counted again"                /flaky/default/ "503 503, 503"
logged "notice when healthy again" 'peer 127.0.0.1:9101 in upstream "hc_default" is healthy'

echo "9101 /health -> 200 \"degraded\""
since=$(date +%s); set_state "touch /tmp/hc/9101.degraded"
expect "match: body ~ ^ok fails, 9101 out"     /match/     9102
expect "neg: body !~ degraded fails, 9101 out" /neg/       9102
now    "default: 2xx passes without match"     /default/   9101,9102
logged "reason names the failed test" 'response fails "body ~ ^ok"'

echo "9101 /health -> 200 text/html"
set_state
expect "match: 9101 healthy first"             /match/     9101,9102
expect "neg: 9101 healthy first"               /neg/       9101,9102
since=$(date +%s); set_state "touch /tmp/hc/9101.html"
expect "match: header = text/plain fails"      /match/     9102
expect "neg: header != text/html fails"        /neg/       9102
now    "default: both servers"                 /default/   9101,9102
logged "reason names the failed test" 'response fails "header Content-Type = text/plain"'

echo "both servers fail"
set_state "touch /tmp/hc/9101.fail /tmp/hc/9102.fail"
expect "default: no healthy server -> 502"     /default/   502
now    "root: unaffected, both"                /root/      9101,9102

echo "both recover"
set_state
expect "default: both servers"                 /default/   9101,9102
expect "match: both servers"                   /match/     9101,9102

# --- two checks of one group ---------------------------------------------------
# hc_two is checked on /health and on /ready: a server is used only while
# it passes both. Its state lives in shared memory, so a new worker 0
# starts from it.

echo "two checks: 9101 fails both"
set_state "touch /tmp/hc/9101.fail /tmp/hc/9101.unready"
expect "two: 9101 taken out"                   /two/ 9102
since=$(date +%s); cx "rm /tmp/hc/9101.fail"
logged "two: notice for the check that passes" \
       'peer 127.0.0.1:9101 in upstream "hc_two" passes health_check in /etc/nginx/nginx.conf:'
now    "two: 9101 still out, fails /ready"     /two/ 9102

echo "two checks: workers restart while 9101 fails /ready"
since=$(date +%s); cx "pkill -f '[n]ginx: worker process'"
logged "restart: new workers started"          'start worker process'
now    "restart: 9101 stays out"               /two/ 9102
sleep 1
now    "restart: 9101 still out"               /two/ 9102
not_logged "restart: state kept, no new warning" \
       'peer 127.0.0.1:9101 in upstream "hc_two" is unhealthy'

echo "two checks: 9101 passes both"
since=$(date +%s); set_state
expect "two: 9101 back"                        /two/ 9101,9102
logged "two: healthy" 'peer 127.0.0.1:9101 in upstream "hc_two" is healthy'

# --- draining server -------------------------------------------------------------
# In hc_drain, 9101 is "drain": only clients bound to it (sticky cookie
# srv=a) get it. It is checked like any other, and an unhealthy one loses
# them too.

echo "drain"
expect "drain: new clients get 9102"           /drain/ 9102
expect "drain: srv=a still gets 9101"          /drain/ 9101 -b srv=a
since=$(date +%s); set_state "touch /tmp/hc/9101.fail"
expect "drain: unhealthy 9101 loses srv=a"     /drain/ 9102 -b srv=a
logged "drain: the draining server is checked" \
       'peer 127.0.0.1:9101 in upstream "hc_drain" is unhealthy'
set_state
expect "drain: srv=a gets 9101 again"          /drain/ 9101 -b srv=a
now    "drain: still draining for new clients" /drain/ 9102

# --- servers from DNS ("resolve") ----------------------------------------------
# nginx re-resolves dyn.test every second (resolver valid=1s).

echo "resolve: dyn.test -> 127.0.0.1, 127.0.0.2"
dns 127.0.0.1 127.0.0.2
expect "dns: both addresses"                   /dns/ 127.0.0.1,127.0.0.2
attempts "dns flaky: two attempts"             /flaky/dns/ "503 503, 503"

echo "resolve: 127.0.0.2 fails"
since=$(date +%s); set_state "touch /tmp/hc/127.0.0.2.fail"
expect "dns: 127.0.0.2 taken out"              /dns/ 127.0.0.1
logged "warn for the resolved server" 'peer 127.0.0.2:9201 in upstream "hc_dns" is unhealthy'
attempts "dns flaky: unhealthy server not counted" /flaky/dns/ "503 503"

echo "resolve: DNS drops the unhealthy 127.0.0.2"
dns 127.0.0.1
sleep 3
now    "dns: only 127.0.0.1"                   /dns/ 127.0.0.1
attempts "dns flaky: still one attempt"        /flaky/dns/ "503 503"

echo "resolve: DNS adds 127.0.0.3"
dns 127.0.0.1 127.0.0.3
expect "dns: new address used"                 /dns/ 127.0.0.1,127.0.0.3
attempts "dns flaky: two attempts"             /flaky/dns/ "503 503, 503"

echo "resolve: DNS re-adds 127.0.0.2, still failing"
since=$(date +%s); dns 127.0.0.1 127.0.0.2 127.0.0.3
logged "re-added server probed and taken out" 'peer 127.0.0.2:9201 in upstream "hc_dns" is unhealthy'
expect "dns: 127.0.0.1 and 127.0.0.3"          /dns/ 127.0.0.1,127.0.0.3
attempts "dns flaky: two attempts"             /flaky/dns/ "503 503, 503"

echo "resolve: 127.0.0.2 recovers"
set_state
expect "dns: all three addresses"              /dns/ 127.0.0.1,127.0.0.2,127.0.0.3
attempts "dns flaky: three attempts"           /flaky/dns/ "503 503, 503, 503"

dns 127.0.0.1 127.0.0.2
set_state

# --- DNS SRV server down by configuration ---------------------------------------
# srv.test service=http -> a.srv.test:9201 = 127.0.0.4. nginx names such a
# server after the SRV target (a.srv.test), not after the configured name.

echo "SRV: srv.test resolve down"
expect "srv control: the SRV record resolves"  /srv-up/ 127.0.0.4
sleep 3
now    "srv: config-down SRV server stays down" /srv/ 9102
is     "srv: config-down SRV server not probed" "$(cx "wc -l < /tmp/srv-health.log")" 0

# --- response framing -------------------------------------------------------------
# 127.0.0.1:9301 (socat) sends /tmp/raw/resp verbatim and closes. While it
# is healthy, requests get its response ("ok", 204, or 502 if nginx's proxy
# rejects it); once it is unhealthy, the backup server 9102 answers. hc_raw
# probes with HTTP/1.0 and proxy_buffer_size 1k, hc_raw11 with HTTP/1.1.

ok_resp='HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n'

echo "response framing"
raw "$ok_resp"
expect "raw: complete response passes"         /raw/ ok
since=$(date +%s)
raw 'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nok\n'
expect "raw: body shorter than Content-Length fails" /raw/ 9102
logged "reason: incomplete body" \
       'peer 127.0.0.1:9301 in upstream "hc_raw" is unhealthy: connection closed before the response body was complete'
raw 'HTTP/1.0 200 OK\r\n\r\nok\n'
expect "raw: close-delimited body passes"      /raw/ ok
since=$(date +%s)
raw 'HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 100\r\n\r\nok\n'
expect "raw: conflicting Content-Length fails" /raw/ 9102
logged "reason: second Content-Length" 'is unhealthy: duplicate Content-Length'
raw 'HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n'
expect "raw: 204 needs no body"                /raw/ 204
raw 'HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nok\n'
expect "raw: repeated Content-Length fails, as for the proxy" /raw/ 9102
raw "$ok_resp"
expect "raw: complete response passes again"   /raw/ ok
raw 'HTTP/1.1 200 OK\r\nContent-Length : 100\r\n\r\nok\n'
expect "raw: whitespace before colon fails"    /raw/ 9102
raw 'HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nok\n'
expect "raw: 103 Early Hints skipped, 200 passes" /raw/ ok
since=$(date +%s)
raw "HTTP/1.1 200 OK\r\nX-Pad: $(printf '%01500d' 0)\r\nContent-Length: 3\r\n\r\nok\n"
expect "raw: header over proxy_buffer_size fails" /raw/ 9102
logged "reason: header too large" 'upstream "hc_raw" is unhealthy: response header too large'
expect "raw11: fits its proxy_buffer_size"     /raw11/ ok
raw "HTTP/1.1 103 Early Hints\r\nLink: <$(printf '%0680d' 0)>; rel=preload\r\n\r\nHTTP/1.1 200 OK\r\nX-Pad: $(printf '%0400d' 0)\r\nContent-Length: 3\r\n\r\nok\n"
expect "raw: each header gets the whole proxy_buffer_size" /raw/ ok
since=$(date +%s)
raw "$(early 17)$ok_resp"
expect "raw: 17 interim responses fail"        /raw/ 9102
logged "reason: too many interim responses" 'upstream "hc_raw" is unhealthy: too many interim responses'
raw "$(early 16)$ok_resp"
expect "raw: 16 interim responses pass"        /raw/ ok
since=$(date +%s)
raw "$(early 1)"
expect "raw: closed after an interim response fails" /raw/ 9102
logged "reason: header incomplete" \
       'upstream "hc_raw" is unhealthy: connection closed before the response header was complete'
raw "$ok_resp"
expect "raw: complete response passes once more" /raw/ ok
since=$(date +%s)
raw ''
expect "raw: closed without a response fails"  /raw/ 9102
logged "reason: no response" 'is unhealthy: connection closed without a response'
raw "$ok_resp"
expect "raw: complete response passes yet again" /raw/ ok
since=$(date +%s)
raw '\025\003\003\000\002\002\106'
expect "raw: TLS alert fails"                  /raw/ 9102
logged "reason: TLS instead of HTTP" 'is unhealthy: TLS data instead of an HTTP response'
raw "$ok_resp"
expect "raw: healthy at the end"               /raw/ ok

# hc_raw11 has match body ~ "^ok$", which the chunks would fail.

echo "chunked responses"
expect "raw11: complete response passes"       /raw11/ ok
since=$(date +%s)
raw 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nok\n\r\n'
expect "raw11: no last chunk fails"            /raw11/ 9102
logged "reason: incomplete chunked body" \
       'peer 127.0.0.1:9301 in upstream "hc_raw11" is unhealthy: connection closed before the response body was complete'
raw 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\no\r\n2\r\nk\n\r\n0\r\n\r\n'
expect "raw11: chunks decoded, healthy again"  /raw11/ ok
expect "raw: chunked for HTTP/1.0 decoded, as by the proxy" /raw/ ok
since=$(date +%s)
raw 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n3\r\nok\n\r\n0\r\n\r\n'
expect "raw11: with Content-Length fails"      /raw11/ 9102
logged "reason: both lengths" 'is unhealthy: both Content-Length and Transfer-Encoding'
raw 'HTTP/1.1 200 OK\r\nTRANSFER-ENCODING: Chunked\r\n\r\n3;a=b\r\nok\n\r\n0\r\nX-Trailer: 1\r\n\r\n'
expect "raw11: extensions, trailers: healthy"  /raw11/ ok
since=$(date +%s)
raw 'HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n'
expect "raw11: other transfer coding fails"    /raw11/ 9102
logged "reason: unsupported coding" 'upstream "hc_raw11" is unhealthy: unsupported Transfer-Encoding'
raw "$ok_resp"
expect "raw11: plain response: healthy"        /raw11/ ok
since=$(date +%s)
raw 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nok\n\r\n0\r\n\r\n'
expect "raw11: invalid chunk size fails"       /raw11/ 9102
logged "reason: invalid chunked body" 'upstream "hc_raw11" is unhealthy: invalid chunked body'
raw "$ok_resp"
expect "raw11: healthy at the end"             /raw11/ ok
expect "raw: healthy at the end too"           /raw/ ok

echo "HTTP version, proxy_bind"
since=0
logged "1.0 probe gets 426 where 1.1 is needed" \
       'peer 127.0.0.1:9101 in upstream "hc_h10" is unhealthy: status 426 is not 2xx or 3xx'
expect "bind: probes come from proxy_bind's address" /bind/ 9101

# --- a server that never answers -------------------------------------------------
# hc_slow has 9302, which never answers, and 9101. Each server is probed on
# its own schedule: 9101's state changes while 9302's probe waits for
# proxy_read_timeout (4s).

echo "a server that never answers"
since=$(date +%s); set_state "touch /tmp/hc/9101.fail"
logged "slow: 9101 taken out without waiting for 9302" \
       'peer 127.0.0.1:9101 in upstream "hc_slow" is unhealthy'
since=0
logged "slow: 9302 fails on proxy_read_timeout" \
       'peer 127.0.0.1:9302 in upstream "hc_slow" is unhealthy: timed out reading the response'
set_state

# --- https --------------------------------------------------------------------
# tls.test: 9401 (needs SNI) with backup 9402, verified against test-ca,
# probed with HTTP/1.1 on /health11 (426 for HTTP/1.0).
# wrong.test and hc_tls_ca fail verification: their probes say why.

echo "https"
expect "tls: SNI and verification pass, 9401"  /tls/ 9401
since=$(date +%s); set_state "touch /tmp/hc/9401.fail"
expect "tls: 9401 taken out, backup 9402"      /tls/ 9402
logged "tls: reason is the status" \
       'peer 127.0.0.1:9401 in upstream "tls.test" is unhealthy: status 503 is not 2xx or 3xx'
since=$(date +%s); set_state
expect "tls: 9401 back"                        /tls/ 9401
logged "tls: healthy again" 'peer 127.0.0.1:9401 in upstream "tls.test" is healthy'
since=0
logged "tls: name mismatch fails" \
       'peer 127.0.0.1:9402 in upstream "wrong.test" is unhealthy: SSL certificate does not match "wrong.test"'
logged "tls: unknown CA fails" \
       'peer 127.0.0.1:9402 in upstream "hc_tls_ca" is unhealthy: SSL certificate verify error'

# --- second nginx instances ----------------------------------------------------------
# More servers than connection slots: 40 probes per round (the same healthy
# server, 40 times), 16 worker_connections. Probes that would not fit must
# wait for a slot, not fail the server.

echo "probes beyond worker_connections"
extra_nginx /tmp/many <<EOF
    upstream many {
        zone many 256k;
$(for _ in {1..40}; do echo "        server 127.0.0.1:9101;"; done)
    }
    server {
        listen 127.0.0.1:18990;
        location / { proxy_pass http://many; health_check interval=1s uri=/health; }
    }
EOF
sleep 4
is "many: server still in use" "$(cx "wget -qO- http://127.0.0.1:18990/ 2>&1")" 9101
is "many: no probe failed for lack of connections" "$(cx "grep -c 'is unhealthy' /tmp/many/error.log")" 0
cx "nginx -c /tmp/many/nginx.conf -s quit"

# A worker that shuts down with a probe in progress (9302 never answers)
# closes it, rather than leaving its socket open.

echo "shutdown with a probe in progress"
extra_nginx /tmp/quit <<EOF
    upstream stall {
        zone stall 256k;
        server 127.0.0.1:9302;
    }
    server {
        listen 127.0.0.1:18991;
        location / { proxy_pass http://stall; health_check interval=1s uri=/health; }
    }
EOF
sleep 2
cx "nginx -c /tmp/quit/nginx.conf -s quit && timeout 10 sh -c 'while [ -f /tmp/quit/nginx.pid ]; do sleep 0.2; done'"
is "quit: exits within 10s"                    "$(cx "[ -f /tmp/quit/nginx.pid ] && echo running || echo exited")" exited
is "quit: no socket left open"                 "$(cx "grep -c 'open socket' /tmp/quit/error.log")" 0

# --- configuration errors ---------------------------------------------------------

# conf_test NAME WANT CONFIG_HTTP_BODY: WANT is "ok" or a substring of the error
conf_test() {
    local name=$1 want=$2 body=$3 out
    out=$("$ENGINE" exec -i "$C" sh -c 'cat > /tmp/t.conf && nginx -t -c /tmp/t.conf 2>&1; rm -f /tmp/t.conf' <<EOF
load_module modules/ngx_http_health_check_module.so;
pid /tmp/t.pid;
events {}
http {
    upstream z { zone z 256k; server 127.0.0.1:1; }
    upstream nz { server 127.0.0.1:1; }
    match m { status 200; }
    $body
}
EOF
)
    if [[ $want == ok ]]; then
        [[ $out == *"test is successful"* ]] && ok "$name" "accepted" || bad "$name" "$out"
    else
        [[ $out == *"$want"* && $out == *"test failed"* ]] && ok "$name" "rejected" \
            || bad "$name" "want error containing: $want; got: $out"
    fi
}
srv() { echo "server { listen 127.0.0.1:18999; $1 }"; }
locs() { local i; for ((i = 0; i < $1; i++)); do echo "location /$i { proxy_pass http://z; health_check; }"; done; }

echo "configuration"
conf_test "all three parameters"                    ok \
    "$(srv 'location / { proxy_pass http://z; health_check interval=10s uri=/hc match=m; }')"
conf_test "bare health_check"                       ok \
    "$(srv 'location / { proxy_pass http://z; health_check; }')"
conf_test "48 checks of one group"                  ok \
    "$(srv "$(locs 48)")"
conf_test "49 checks of one group"                  'too many health checks for upstream "z"' \
    "$(srv "$(locs 49)")"
conf_test "upstream without zone"                   'must reside in shared memory' \
    "$(srv 'location / { proxy_pass http://nz; health_check; }')"
conf_test "unknown match"                           'match "nope" not found' \
    "$(srv 'location / { proxy_pass http://z; health_check match=nope; }')"
conf_test "proxy_pass with a variable"              'needs "proxy_pass" to an upstream group' \
    "$(srv 'location / { proxy_pass http://$host; health_check; }')"
conf_test "no proxy_pass"                           'needs "proxy_pass" to an upstream group' \
    "$(srv 'location / { health_check; }')"
conf_test "https upstream"                          ok \
    "$(srv 'location / { proxy_pass https://z; health_check; }')"
conf_test "https: proxy_ssl_name with a variable"   'with variables' \
    "$(srv 'location / { proxy_pass https://z; proxy_ssl_name $host; health_check; }')"
conf_test "https: certificate with a variable"      'with variables' \
    "$(srv 'location / { proxy_pass https://z; proxy_ssl_certificate /c/$host.crt; proxy_ssl_certificate_key /c/$host.key; health_check; }')"
conf_test "proxy_bind with a variable"              ok \
    "$(srv 'location / { proxy_pass http://z; proxy_bind $remote_addr; health_check; }')"
conf_test "unsupported parameter fails="            '"fails=3" is not supported' \
    "$(srv 'location / { proxy_pass http://z; health_check fails=3; }')"
conf_test "unsupported parameter mandatory"         '"mandatory" is not supported' \
    "$(srv 'location / { proxy_pass http://z; health_check mandatory; }')"
conf_test "invalid interval"                        'invalid parameter "interval=soon"' \
    "$(srv 'location / { proxy_pass http://z; health_check interval=soon; }')"
conf_test "uri with a space"                        'invalid parameter "uri=/a b"' \
    "$(srv 'location / { proxy_pass http://z; health_check "uri=/a b"; }')"
conf_test "uri without a slash"                     'invalid parameter "uri=hc"' \
    "$(srv 'location / { proxy_pass http://z; health_check uri=hc; }')"
conf_test "health_check outside location"           '"health_check" directive is not allowed here' \
    "$(srv 'health_check;')"
conf_test "match: require not supported"            '"require" in "match" is not supported' \
    'match r { require $host; }'
conf_test "match: invalid status"                   'invalid status "600"' \
    'match r { status 600; }'
conf_test "match: invalid operator"                 'invalid operator "=="' \
    'match r { header X == y; }'
conf_test "match: no = for the body"                'invalid operator "="' \
    'match r { body = y; }'
conf_test "match: invalid regex"                    'pcre2_compile() failed' \
    'match r { body ~ "("; }'
conf_test "match: duplicate name"                   'duplicate match "m"' \
    'match m { status 200; }'

is "log: no alert, crit or emerg" \
   "$("$ENGINE" logs "$C" 2>&1 | grep -E '\[(alert|crit|emerg)\]')" ""

echo
echo "$pass passed, $fail failed"
exit "$fail"
