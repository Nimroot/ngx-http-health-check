#!/usr/bin/env bash
# Black-box tests against the test container (see the Makefile: make test).
# Breaks and repairs backends (files in /tmp/hc/), changes DNS answers
# (/tmp/dns/hosts), and checks where requests go. Then checks configuration
# errors with `nginx -t`. Exit code = number of failures.
set -uo pipefail

BASE="${BASE:-http://127.0.0.1:18082}"
C="${CONTAINER:-ngx-health-check-test}"
ENGINE="${ENGINE:-podman}"
WAIT="${WAIT:-8}"      # seconds to wait for a state change (checks run every 1s)

pass=0 fail=0
ok()  { printf '  \e[32mPASS\e[0m %-56s %s\n' "$1" "$2"; ((pass++)); }
bad() { printf '  \e[31mFAIL\e[0m %-56s %s\n' "$1" "$2"; ((fail++)); }

cx() { "$ENGINE" exec "$C" sh -c "$1"; }
set_state() { cx "mkdir -p /tmp/hc && rm -f /tmp/hc/* && ${1:-true}"; }

# dns ADDR...: dyn.test resolves to these addresses from now on
dns() {
    "$ENGINE" exec "$C" sh -c \
        'for a in "$@"; do echo "$a dyn.test"; done > /tmp/dns/hosts && pkill -HUP dnsmasq' \
        sh "$@"
}

# served PATH: which backends answer 8 requests, e.g. "9101,9102", "9102", "502"
served() {
    local i out
    for i in {1..8}; do
        out=$(curl -s -w ' %{http_code}' "$BASE$1")
        out=${out//$'\n'/}
        if [[ $out == *" 200" ]]; then echo "${out% *}"; else echo "${out##* }"; fi
    done | sort -u | paste -sd, -
}

# expect NAME PATH WANT: waits up to $WAIT seconds for PATH to be served by WANT
expect() {
    local name=$1 path=$2 want=$3 got start=$SECONDS
    while :; do
        got=$(served "$path")
        [[ $got == "$want" ]] && { ok "$name" "$got ($((SECONDS - start))s)"; return; }
        ((SECONDS - start >= WAIT)) && { bad "$name" "got $got, want $want"; return; }
        sleep 0.5
    done
}

# now NAME PATH WANT: checked once, no waiting (state that must NOT change)
now() {
    local got; got=$(served "$2")
    [[ $got == "$3" ]] && ok "$1" "$got" || bad "$1" "got $got, want $3"
}

# attempts NAME PATH WANT: status and $upstream_status of a /flaky/ request,
# e.g. "503 503, 503" = two attempts, both answered 503
attempts() {
    local got
    got=$(curl -s -o /dev/null -w '%{http_code} %header{x-upstream-status}' "$BASE$2")
    [[ $got == "$3" ]] && ok "$1" "$got" || bad "$1" "got '$got', want '$3'"
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

for _ in {1..40}; do curl -sf "$BASE/healthz" >/dev/null && break; sleep 0.25; done

echo "all healthy"
set_state
expect "default: both servers"                 /default/   9101,9102
expect "match: both servers"                   /match/     9101,9102
expect "root: both servers"                    /root/      9101,9102
now    "admin: 9101 is down by config"         /admin/     9102
now    "unchecked: both servers"               /unchecked/ 9101,9102
attempts "flaky: one attempt per server"       /flaky/default/ "503 503, 503"

echo "9101 /health -> 503"
since=$(date +%s); set_state "touch /tmp/hc/9101.fail"
expect "default: 9101 taken out"               /default/   9102
expect "match: 9101 taken out"                 /match/     9102
now    "root: uri=/ still passes, both"        /root/      9101,9102
now    "unchecked: untouched, both"            /unchecked/ 9101,9102
attempts "flaky: unhealthy server not counted" /flaky/default/ "503 503"
logged "warn names peer, upstream, reason" \
       'peer 127.0.0.1:9101 in upstream "hc_default" is unhealthy: status 503 is not 2xx or 3xx'

echo "9101 recovers"
since=$(date +%s); set_state
expect "default: 9101 back"                    /default/   9101,9102
expect "match: 9101 back"                      /match/     9101,9102
now    "admin: config-down 9101 stays down"    /admin/     9102
attempts "flaky: counted again"                /flaky/default/ "503 503, 503"
logged "notice when healthy again" 'peer 127.0.0.1:9101 in upstream "hc_default" is healthy'

echo "9101 /health -> 200 \"degraded\" (fails body ~ \"^ok\")"
since=$(date +%s); set_state "touch /tmp/hc/9101.degraded"
expect "match: 9101 taken out"                 /match/     9102
now    "default: 2xx passes without match"     /default/   9101,9102
logged "reason names the failed test" 'response fails "body ~ ^ok"'

echo "9101 /health -> 200 text/html (fails header Content-Type = text/plain)"
set_state
expect "match: 9101 healthy first"             /match/     9101,9102
since=$(date +%s); set_state "touch /tmp/hc/9101.html"
expect "match: 9101 taken out"                 /match/     9102
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
cx "rm -rf /tmp/hc"

# --- DNS SRV server down by configuration ---------------------------------------
# srv.test service=http -> a.srv.test:9201 = 127.0.0.4. nginx names such a
# server after the SRV target (a.srv.test), not after the configured name.

echo "SRV: srv.test resolve down"
expect "srv control: the SRV record resolves"  /srv-up/ 127.0.0.4
sleep 3
now    "srv: config-down SRV server stays down" /srv/ 9102
got=$(cx "wc -l < /tmp/srv-health.log")
[[ $got == 0 ]] && ok "srv: config-down SRV server not probed" "0 probes" \
                || bad "srv: config-down SRV server not probed" "$got probes"

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

echo "configuration"
conf_test "all three parameters"                    ok \
    "$(srv 'location / { proxy_pass http://z; health_check interval=10s uri=/hc match=m; }')"
conf_test "bare health_check"                       ok \
    "$(srv 'location / { proxy_pass http://z; health_check; }')"
conf_test "upstream without zone"                   'must reside in shared memory' \
    "$(srv 'location / { proxy_pass http://nz; health_check; }')"
conf_test "unknown match"                           'match "nope" not found' \
    "$(srv 'location / { proxy_pass http://z; health_check match=nope; }')"
conf_test "proxy_pass with a variable"              'needs "proxy_pass" to an upstream group' \
    "$(srv 'location / { proxy_pass http://$host; health_check; }')"
conf_test "no proxy_pass"                           'needs "proxy_pass" to an upstream group' \
    "$(srv 'location / { health_check; }')"
conf_test "https upstream"                          'over https is not supported' \
    "$(srv 'location / { proxy_pass https://z; health_check; }')"
conf_test "unsupported parameter fails="            '"fails=3" is not supported' \
    "$(srv 'location / { proxy_pass http://z; health_check fails=3; }')"
conf_test "unsupported parameter mandatory"         '"mandatory" is not supported' \
    "$(srv 'location / { proxy_pass http://z; health_check mandatory; }')"
conf_test "invalid interval"                        'invalid "interval" value' \
    "$(srv 'location / { proxy_pass http://z; health_check interval=soon; }')"
conf_test "invalid uri"                             'invalid "uri" value' \
    "$(srv 'location / { proxy_pass http://z; health_check "uri=/a b"; }')"
conf_test "health_check outside location"           '"health_check" directive is not allowed here' \
    "$(srv 'health_check;')"
conf_test "match: require not supported"            '"require" in "match" is not supported' \
    'match r { require $host; }'
conf_test "match: invalid status"                   'invalid status "600"' \
    'match r { status 600; }'
conf_test "match: invalid regex"                    'pcre2_compile() failed' \
    'match r { body ~ "("; }'
conf_test "match: duplicate name"                   'duplicate match "m"' \
    'match m { status 200; }'

echo
echo "$pass passed, $fail failed"
exit "$fail"
