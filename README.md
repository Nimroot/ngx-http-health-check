# ngx_http_health_check

Active health checks for open-source nginx, configured with the `health_check`
and `match` directives of the NGINX Plus
[ngx_http_upstream_hc_module](https://nginx.org/en/docs/http/ngx_http_upstream_hc_module.html),
which this module reimplements in part. A configuration that uses the
supported subset works unchanged on both.

> **Status: experimental.** Tested with nginx 1.29.8 and 1.30.6 on Linux, in
> the scenarios under [Testing](#testing). Review it before relying on it in
> production.

```nginx
load_module modules/ngx_http_health_check_module.so;

http {
    upstream backend {
        zone backend 256k;
        server 10.0.0.1:8080;
        server 10.0.0.2:8080;
    }

    match backend_ok {
        status 200;
        header Content-Type = application/json;
        body ~ '"status": ?"ok"';
    }

    server {
        location / {
            proxy_pass http://backend;
            health_check interval=5s uri=/health match=backend_ok;
        }
    }
}
```

Every 5 seconds each server gets `GET /health`. A server whose response fails
the `match` tests, or that can't be reached in time, gets no requests until a
check passes again. If every server in the group is unhealthy, requests get 502.

## Requirements

- nginx **1.29.4 or later** (it uses `ngx_http_proxy_module.h`, added in 1.29.4).
  Older versions stop the build with an explicit error.
- PCRE (for `~` tests) and the upstream zone module, both part of a default
  build.
- An upstream group with a `zone`, used with `proxy_pass` (`http://` or
  `https://`).
- For `https://`, nginx and the module built with `--with-http_ssl_module`.

## Building

As a dynamic module, against the same nginx version you run:

```sh
cd nginx-1.30.6
./auto/configure --with-compat --add-dynamic-module=/path/to/ngx_http_health_check
make modules
cp objs/ngx_http_health_check_module.so /usr/lib/nginx/modules/
```

Then `load_module modules/ngx_http_health_check_module.so;` at the top of
`nginx.conf`. Building it in statically with `--add-module=` also works.

## Directives

### health_check

    Syntax:  health_check [interval=time] [uri=uri] [match=name];
    Context: location

Checks the servers of the group that the location's `proxy_pass` points to.
The group must have a `zone`.

| Parameter | Default | |
|---|---|---|
| `interval=time` | `5s` | time between two checks of a server |
| `uri=uri` | `/` | URI of the check request |
| `match=name` | status 2xx or 3xx | `match` block the response must pass |

Without `match`, a check passes if the server answers with a 2xx or 3xx
status. Several `health_check`s may check the same group, from different
locations: a server is healthy only while all of them pass.

### match

    Syntax:  match name { ... }
    Context: http

Tests a check response must pass, all of them:

| Test | Passes if |
|---|---|
| `status 200;` | status is 200 |
| `status ! 500;` | status is not 500 |
| `status 200 204;` | status is 200 or 204 |
| `status 200-399;` | status is in the range |
| `status ! 400-599;` | status is outside the range |
| `status 301-303 307;` | status is 301, 302, 303 or 307 |
| `header Name;` | the header is present |
| `header ! Name;` | the header is absent |
| `header Name = value;` | a `Name` header has exactly this value |
| `header Name != value;` | a `Name` header has another value |
| `header Name ~ regex;` | a `Name` header matches the regex |
| `header Name !~ regex;` | a `Name` header doesn't match the regex |
| `body ~ regex;` | the body matches the regex |
| `body !~ regex;` | the body doesn't match the regex |

Header names are case-insensitive. Only the first 256k of the body is examined.

## How it works

- Because of the `zone`, the group's servers live in shared memory, seen by
  every worker. Worker 0 runs all checks.
- A check opens a new connection to each server and sends `GET <uri>` with
  the location's `proxy_http_version`: `HTTP/1.1` (nginx's default) or
  `HTTP/1.0`; with `proxy_http_version 2`, `HTTP/1.1`. `Host:` is set to the
  group name (Plus's default `$proxy_host`), plus `Connection: close`. It
  uses the location's `proxy_connect_timeout`, `proxy_send_timeout` and
  `proxy_read_timeout`.
- With `proxy_pass https://`, the check does a TLS handshake first, with the
  location's settings as requests do: `proxy_ssl_protocols`,
  `proxy_ssl_ciphers`, `proxy_ssl_certificate` and
  `proxy_ssl_certificate_key`, `proxy_ssl_trusted_certificate` (such as a
  private CA), `proxy_ssl_verify`, `proxy_ssl_server_name` (SNI) and
  `proxy_ssl_name`, which defaults to the group name. The handshake must be
  done within `proxy_connect_timeout`. A failed handshake or certificate
  check fails the check, with the reason in the log. Each check does a full
  handshake (no session reuse).
- The result sets the server's `down` flag in shared memory. Every
  load-balancing method (round robin, `hash`, `ip_hash`, `least_conn`,
  `random`) already skips down servers. The module also keeps the group's
  count of usable servers in step, as nginx does for servers marked `down` in
  the configuration, so `proxy_next_upstream` makes one attempt per healthy
  server.
- Servers marked `down` in the configuration are never checked or brought up.
  That includes servers added by `resolve`, also from SRV records
  (`service=`), whose names are those of the records' targets.
- A response passes only if it is complete: a response with status 1xx, 204
  or 304 ends with its header; any other body ends after `Content-Length`
  bytes (or the first 256k of them), with the last chunk of a
  `Transfer-Encoding: chunked` body (in answer to HTTP/1.1; `match` sees the
  decoded data), or when the server closes the connection if there is
  neither. As with nginx's proxy, a connection closed early,
  `Content-Length` headers with different values, `Content-Length` with
  `Transfer-Encoding`, a transfer coding other than `chunked` alone, an
  invalid chunk, or a header line with space before its colon fail the
  check.
- At most a quarter of `worker_connections` probes run at once; the others
  wait for their turn. A probe that worker 0 cannot carry out for a reason
  of its own (no free connection, out of memory) leaves the server's state
  as it was and logs an `error`.
- Servers start healthy. That includes servers added by `resolve`, and
  everything after a reload or a restart of worker 0.
- A change of state is logged: `warn` with the reason, `notice` when the
  server is healthy again:

      peer 10.0.0.2:8080 in upstream "backend" is unhealthy: status 503 fails "status 200" (health_check in /etc/nginx/nginx.conf:17)
      peer 10.0.0.2:8080 in upstream "backend" is healthy

## Differences from NGINX Plus

| | NGINX Plus | This module |
|---|---|---|
| `fails=`, `passes=` | default 1 | always 1; setting them is a config error |
| `jitter`, `mandatory`, `persistent`, `port`, `type=grpc`, `keepalive_time` | supported | config error |
| `require` in `match` | supported | config error |
| `proxy_pass https://` | supported | supported; `proxy_ssl_name` and `proxy_ssl_certificate` with variables are a config error |
| `fastcgi_pass`, `grpc_pass`, `uwsgi_pass`, ... | supported | `proxy_pass` only |
| Check request | uses the location's `proxy_set_header` and `proxy_http_version` | uses `proxy_http_version` (HTTP/1.x only); fixed headers, see [How it works](#how-it-works) |
| Who runs the checks | the workers | worker 0 |
| Health state | API and dashboard | log only |

The NGINX Plus column comes from the
[ngx_http_upstream_hc_module documentation](https://nginx.org/en/docs/http/ngx_http_upstream_hc_module.html),
not from testing against NGINX Plus.

## Limitations

- **Worker 0 runs all checks.** If it is busy or stuck, checks are delayed.
- **The response must be HTTP/1.x**, with no transfer coding other than
  `chunked` (none at all in answer to HTTP/1.0).
- **The response header is limited to 32k.** A larger one fails the check.
- **No variables:** a check has no request to evaluate them for, so
  `proxy_ssl_name` and `proxy_ssl_certificate` (and its key) must be fixed
  strings.
- **https:** ALPN isn't sent.
- **Zone size:** nginx's minimum for an upstream `zone` depends on the memory
  page size. The 64k used in many examples is too small on systems with 16k
  pages (some arm64 systems); 256k works there.
- **Compatibility:** the module reads and writes nginx's internal upstream
  structures, which can change between nginx versions. Run the tests before
  upgrading.

## Source layout

| File | |
|---|---|
| `config` | nginx build script for `--add-module` / `--add-dynamic-module` |
| `src/ngx_http_health_check_module.c` | module definition, `health_check` directive, configuration checks |
| `src/ngx_http_health_check_match.c` | `match` block: parsing its tests and running them against a response |
| `src/ngx_http_health_check_probe.c` | starting checks in worker 0, probing servers (connect, send, read, parse) and recording their health |
| `src/ngx_http_health_check.h` | types shared by the source files |
| `t/` | test container and black-box tests (see below) |

## Testing

`make test` builds nginx with the module in a container (Alpine, from source),
starts it, and runs `t/test.sh`, which takes backends down and up and checks
where requests go:

- unhealthy servers are taken out and come back; `uri=` and `match=` (status,
  header and body tests, negated or not) decide health; the default URI is
  `/`;
- groups without `health_check` and servers that are `down` in the
  configuration are left alone; with no healthy server, requests get 502;
- `proxy_next_upstream` counts only healthy servers;
- with two checks of one group, a server is used only while it passes both;
- with `resolve`, servers added by DNS are checked, and removing an unhealthy
  server leaves the count of usable servers correct (dnsmasq in the container
  serves the changing answers); an SRV server marked `down` gets neither
  probes nor requests;
- incomplete or ambiguously delimited responses fail, served byte for byte by
  socat; responses without a body or delimited by closing pass; a connection
  closed without a response, or TLS data in answer to `http://`, fail with a
  reason saying so;
- HTTP/1.1 checks: chunked bodies decoded for `match`, incomplete or
  invalid ones failing; an HTTP/1.0 check failing on a server that answers
  HTTP/1.0 with 426, as Envoy does by default;
- `https://` checks: SNI, verification against a private CA, and failures
  for a certificate for another name or from another CA;
- more servers than free connections in worker 0 don't make any check fail;
- log messages; and `nginx -t` accepts valid configurations and rejects each
  unsupported or invalid one with a clear message.

```sh
make test                                   # podman, nginx 1.30.6
make test ENGINE=docker NGINX_VERSION=1.29.8
```

Each run uses a container of its own and a free port, so runs can go side
by side. Requirements: podman or docker, curl, bash.

## License

BSD 2-Clause, see [LICENSE](LICENSE). nginx itself is under the same license.

NGINX and NGINX Plus are trademarks of F5, Inc. This project is not affiliated
with, endorsed by, or sponsored by F5, Inc. The directive names follow the
NGINX Plus documentation so configurations work on both.
