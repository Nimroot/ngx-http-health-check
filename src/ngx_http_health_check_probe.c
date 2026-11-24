/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Probing the servers of a group and recording their health: worker 0
 * connects to each server, sends the check request, reads and parses the
 * response, has it judged by the match tests, and sets the server's "down"
 * flag in shared memory.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_health_check.h"
#include "ngx_http_health_check_match.h"
#include "ngx_http_health_check_probe.h"


#define NGX_HTTP_HEALTH_CHECK_BODY_MAX    (256 * 1024)
#define NGX_HTTP_HEALTH_CHECK_HEADER_MAX  (32 * 1024)
#define NGX_HTTP_HEALTH_CHECK_BUF_MAX                                        \
    (NGX_HTTP_HEALTH_CHECK_HEADER_MAX + NGX_HTTP_HEALTH_CHECK_BODY_MAX)
#define NGX_HTTP_HEALTH_CHECK_BUF_INIT    4096


typedef struct ngx_http_health_check_probe_s  ngx_http_health_check_probe_t;

/* One check of one server, in its own pool. */
struct ngx_http_health_check_probe_s {
    ngx_http_health_check_t            *check;
    ngx_pool_t                         *pool;
    ngx_peer_connection_t               pc;
    struct sockaddr                    *sockaddr;
    socklen_t                           socklen;
    ngx_str_t                           name;
    u_char                             *sent;

    ngx_buf_t                           buf;
    ngx_uint_t                          status;
    size_t                              header_len;
    off_t                               content_length;
    /* ngx_http_health_check_header_t */
    ngx_array_t                        *headers;
    /* why ngx_http_health_check_parse() rejected the response */
    u_char                             *error;

    unsigned                            bodyless:1;

    ngx_http_health_check_probe_t      *next;
};


static void ngx_http_health_check_prune(ngx_http_health_check_upstream_t *up,
    ngx_http_health_check_probe_t *list);
static void ngx_http_health_check_connect(ngx_http_health_check_probe_t *probe);
static void ngx_http_health_check_send(ngx_event_t *wev);
static void ngx_http_health_check_recv(ngx_event_t *rev);
static void ngx_http_health_check_dummy(ngx_event_t *ev);
static ngx_int_t ngx_http_health_check_parse(
    ngx_http_health_check_probe_t *probe, ngx_uint_t store);
static void ngx_http_health_check_evaluate(
    ngx_http_health_check_probe_t *probe);
static void ngx_http_health_check_finish(ngx_http_health_check_probe_t *probe,
    u_char *reason);
static ngx_uint_t ngx_http_health_check_admin_down(
    ngx_http_health_check_upstream_t *up, ngx_http_upstream_rr_peer_t *peer);


/*
 * Whether a server is down by configuration ("down" on its server line)
 * rather than by its health.
 *
 * A server added by "resolve" is a copy of its server line's template, whose
 * state nobody but the configuration changes; its name can't identify the
 * server line, as for an SRV record it is the record's target. Any other
 * server comes from a server line without "resolve", identified by its name
 * and one of its addresses.
 */

static ngx_uint_t
ngx_http_health_check_admin_down(ngx_http_health_check_upstream_t *up,
    ngx_http_upstream_rr_peer_t *peer)
{
    ngx_uint_t                    i, j;
    ngx_http_upstream_server_t  **server;

    if (!peer->down) {
        return 0;
    }

    if (peer->host) {
        return peer->host->peer && peer->host->peer->down;
    }

    server = up->down.elts;
    for (i = 0; i < up->down.nelts; i++) {

        if (server[i]->name.len != peer->server.len
            || ngx_strncmp(server[i]->name.data, peer->server.data,
                           peer->server.len)
               != 0)
        {
            continue;
        }

        for (j = 0; j < server[i]->naddrs; j++) {
            if (ngx_cmp_sockaddr(server[i]->addrs[j].sockaddr,
                                 server[i]->addrs[j].socklen,
                                 peer->sockaddr, peer->socklen, 1)
                == NGX_OK)
            {
                return 1;
            }
        }
    }

    return 0;
}


/* Timer: probe every server of the group, primary and backup. */

void
ngx_http_health_check_run(ngx_event_t *ev)
{
    ngx_pool_t                        *pool;
    ngx_http_upstream_rr_peer_t       *peer;
    ngx_http_upstream_rr_peers_t      *peers;
    ngx_http_health_check_t           *check;
    ngx_http_health_check_probe_t     *probe, *list;
    ngx_http_health_check_upstream_t  *up;

    check = ev->data;
    up = check->upstream;

    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    list = NULL;

    for (peers = up->uscf->peer.data; peers; peers = peers->next) {

        ngx_http_upstream_rr_peers_rlock(peers);

        for (peer = peers->peer; peer; peer = peer->next) {

            if (ngx_http_health_check_admin_down(up, peer)) {
                continue;
            }

            pool = ngx_create_pool(1024, ev->log);
            if (pool == NULL) {
                continue;
            }

            probe = ngx_pcalloc(pool, sizeof(ngx_http_health_check_probe_t));
            if (probe == NULL) {
                ngx_destroy_pool(pool);
                continue;
            }

            probe->pool = pool;
            probe->check = check;
            probe->socklen = peer->socklen;
            probe->sockaddr = ngx_palloc(pool, peer->socklen);
            probe->name.len = peer->name.len;
            probe->name.data = ngx_pnalloc(pool, peer->name.len);

            if (probe->sockaddr == NULL || probe->name.data == NULL) {
                ngx_destroy_pool(pool);
                continue;
            }

            ngx_memcpy(probe->sockaddr, peer->sockaddr, peer->socklen);
            ngx_memcpy(probe->name.data, peer->name.data, peer->name.len);

            probe->next = list;
            list = probe;
            check->pending++;
        }

        ngx_http_upstream_rr_peers_unlock(peers);
    }

    ngx_http_health_check_prune(up, list);

    if (list == NULL) {
        ngx_add_timer(ev, check->interval);
        return;
    }

    /* the next round is scheduled when the last probe finishes */

    while (list) {
        probe = list;
        list = list->next;
        ngx_http_health_check_connect(probe);
    }
}


/*
 * Forgets the state of addresses that are no longer in the group, such as
 * servers removed by "resolve". Otherwise the list would only ever grow.
 */

static void
ngx_http_health_check_prune(ngx_http_health_check_upstream_t *up,
    ngx_http_health_check_probe_t *list)
{
    ngx_uint_t                      i;
    ngx_http_health_check_peer_t   *st;
    ngx_http_health_check_probe_t  *probe;

    st = up->peers->elts;
    i = 0;

    while (i < up->peers->nelts) {

        for (probe = list; probe; probe = probe->next) {
            if (ngx_cmp_sockaddr(st[i].sockaddr, st[i].socklen,
                                 probe->sockaddr, probe->socklen, 1)
                == NGX_OK)
            {
                break;
            }
        }

        if (probe) {
            i++;
            continue;
        }

        ngx_free(st[i].sockaddr);
        st[i] = st[--up->peers->nelts];
    }
}


static void
ngx_http_health_check_connect(ngx_http_health_check_probe_t *probe)
{
    ngx_int_t                 rc;
    ngx_connection_t         *c;
    ngx_peer_connection_t    *pc;
    ngx_http_health_check_t  *check;

    check = probe->check;
    pc = &probe->pc;

    pc->sockaddr = probe->sockaddr;
    pc->socklen = probe->socklen;
    pc->name = &probe->name;
    pc->get = ngx_event_get_peer;
    pc->log = check->timer.log;
    pc->log_error = NGX_ERROR_INFO;

    rc = ngx_event_connect_peer(pc);

    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        ngx_http_health_check_finish(probe, (u_char *) "connect() failed");
        return;
    }

    c = pc->connection;
    c->data = probe;
    c->pool = probe->pool;
    c->read->handler = ngx_http_health_check_recv;
    c->write->handler = ngx_http_health_check_send;

    /* don't hold up a worker that is shutting down */
    c->read->cancelable = 1;
    c->write->cancelable = 1;

    probe->sent = check->request.data;

    ngx_add_timer(c->write, check->connect_timeout);

    if (rc == NGX_OK) {
        ngx_http_health_check_send(c->write);
    }
}


static void
ngx_http_health_check_send(ngx_event_t *wev)
{
    ssize_t                         n;
    u_char                         *last;
    ngx_connection_t               *c;
    ngx_http_health_check_t        *check;
    ngx_http_health_check_probe_t  *probe;

    c = wev->data;
    probe = c->data;
    check = probe->check;

    if (wev->timedout) {
        ngx_http_health_check_finish(probe, (u_char *)
                                     "timed out connecting or sending");
        return;
    }

    last = check->request.data + check->request.len;

    while (probe->sent < last) {
        n = c->send(c, probe->sent, last - probe->sent);

        if (n == NGX_ERROR) {
            ngx_http_health_check_finish(probe, (u_char *)
                                         "connect() or send() failed");
            return;
        }

        if (n == NGX_AGAIN) {
            if (!wev->timer_set) {
                ngx_add_timer(wev, check->send_timeout);
            }

            if (ngx_handle_write_event(wev, 0) != NGX_OK) {
                ngx_http_health_check_finish(probe, (u_char *) "send() failed");
            }

            return;
        }

        probe->sent += n;
    }

    if (wev->timer_set) {
        ngx_del_timer(wev);
    }

    wev->handler = ngx_http_health_check_dummy;

    ngx_add_timer(c->read, check->read_timeout);

    if (c->read->ready) {
        ngx_http_health_check_recv(c->read);
        return;
    }

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_http_health_check_finish(probe, (u_char *) "recv() failed");
    }
}


static void
ngx_http_health_check_recv(ngx_event_t *rev)
{
    u_char                         *p;
    size_t                          size, body;
    ssize_t                         n;
    ngx_buf_t                      *b;
    ngx_int_t                       rc;
    ngx_connection_t               *c;
    ngx_http_health_check_probe_t  *probe;

    c = rev->data;
    probe = c->data;
    b = &probe->buf;

    if (rev->timedout) {
        ngx_http_health_check_finish(probe,
                                     (u_char *) "timed out reading response");
        return;
    }

    if (b->start == NULL) {
        b->start = ngx_palloc(probe->pool, NGX_HTTP_HEALTH_CHECK_BUF_INIT);
        if (b->start == NULL) {
            ngx_http_health_check_finish(probe, (u_char *) "out of memory");
            return;
        }

        b->pos = b->start;
        b->last = b->start;
        b->end = b->start + NGX_HTTP_HEALTH_CHECK_BUF_INIT;
    }

    for ( ;; ) {

        if (b->last == b->end) {
            size = b->end - b->start;

            if (size >= NGX_HTTP_HEALTH_CHECK_BUF_MAX) {
                /*
                 * Only the first 256k of the body are examined. The header
                 * is at most 32k, so the buffer holds at least that much.
                 */
                ngx_http_health_check_evaluate(probe);
                return;
            }

            size = ngx_min(size * 2, NGX_HTTP_HEALTH_CHECK_BUF_MAX);

            p = ngx_palloc(probe->pool, size);
            if (p == NULL) {
                ngx_http_health_check_finish(probe, (u_char *) "out of memory");
                return;
            }

            b->last = ngx_cpymem(p, b->start, b->last - b->start);
            b->start = p;
            b->pos = p;
            b->end = p + size;
        }

        n = c->recv(c, b->last, b->end - b->last);

        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(rev, 0) != NGX_OK) {
                ngx_http_health_check_finish(probe, (u_char *) "recv() failed");
            }
            return;
        }

        if (n == NGX_ERROR) {
            /*
             * A failed non-blocking connect() can be reported on the read
             * event before the write event has run: nothing was sent yet.
             */
            ngx_http_health_check_finish(probe,
                probe->sent == probe->check->request.data
                ? (u_char *) "connect() failed" : (u_char *) "recv() failed");
            return;
        }

        if (n == 0) {
            /* evaluate() rejects a body shorter than its Content-Length */
            ngx_http_health_check_evaluate(probe);
            return;
        }

        b->last += n;

        /*
         * Done early once the header is complete for a response without
         * a body, or once Content-Length says the body is complete (or
         * at least 256k of it are here). Otherwise the body ends when the
         * server closes the connection.
         */

        rc = ngx_http_health_check_parse(probe, 0);

        if (rc == NGX_ERROR) {
            ngx_http_health_check_finish(probe, probe->error);
            return;
        }

        if (rc == NGX_OK) {
            body = b->last - b->start - probe->header_len;

            if (probe->bodyless
                || (probe->content_length >= 0
                    && (off_t) body
                       >= ngx_min(probe->content_length,
                                  NGX_HTTP_HEALTH_CHECK_BODY_MAX)))
            {
                ngx_http_health_check_evaluate(probe);
                return;
            }
        }
    }
}


static void
ngx_http_health_check_dummy(ngx_event_t *ev)
{
}


/*
 * Parses the status line and header from the buffer. NGX_AGAIN until the
 * empty line ending the header has arrived, NGX_ERROR (with probe->error)
 * for a response that is invalid or whose body can't be delimited, NGX_ABORT
 * if out of memory. With "store", also collects the header lines (pointing
 * into the buffer, so only once reading is over).
 *
 * The body of a response with status 1xx, 204 or 304 is empty, whatever
 * its header says (RFC 9112, 6.3). Any other body is delimited by
 * Content-Length or by the end of the connection: Transfer-Encoding (such
 * as chunked, which a server must not use in answer to an HTTP/1.0 request)
 * isn't supported, and several different Content-Length values, or a field
 * name the server may have meant as Content-Length but isn't, make the
 * length ambiguous.
 */

static ngx_int_t
ngx_http_health_check_parse(ngx_http_health_check_probe_t *probe,
    ngx_uint_t store)
{
    off_t                            len;
    u_char                          *p, *q, *line, *end, *nl, *colon, *v;
    ngx_buf_t                       *b;
    ngx_uint_t                       first;
    ngx_http_health_check_header_t  *h;

    b = &probe->buf;
    p = b->start;
    first = 1;

    probe->content_length = -1;
    probe->bodyless = 0;
    probe->error = (u_char *) "invalid response header";

    if (store) {
        probe->headers = ngx_array_create(probe->pool, 8,
                                    sizeof(ngx_http_health_check_header_t));
        if (probe->headers == NULL) {
            return NGX_ABORT;
        }
    }

    for ( ;; ) {
        nl = ngx_strlchr(p, b->last, LF);

        if (nl == NULL) {
            if ((size_t) (b->last - b->start)
                >= NGX_HTTP_HEALTH_CHECK_HEADER_MAX)
            {
                goto too_large;
            }

            return NGX_AGAIN;
        }

        if ((size_t) (nl + 1 - b->start) > NGX_HTTP_HEALTH_CHECK_HEADER_MAX) {
            goto too_large;
        }

        line = p;
        end = (nl > line && nl[-1] == CR) ? nl - 1 : nl;
        p = nl + 1;

        if (first) {
            /* HTTP/1.x SSS ... */
            if (end - line < 12 || ngx_strncmp(line, "HTTP/", 5) != 0
                || line[8] != ' ')
            {
                probe->error = (u_char *) "invalid status line";
                return NGX_ERROR;
            }

            if (line[9] < '1' || line[9] > '5'
                || line[10] < '0' || line[10] > '9'
                || line[11] < '0' || line[11] > '9')
            {
                probe->error = (u_char *) "invalid status line";
                return NGX_ERROR;
            }

            probe->status = (line[9] - '0') * 100 + (line[10] - '0') * 10
                            + (line[11] - '0');

            probe->bodyless = (probe->status < 200 || probe->status == 204
                               || probe->status == 304);

            first = 0;
            continue;
        }

        if (end == line) {
            probe->header_len = p - b->start;
            probe->error = NULL;
            return NGX_OK;
        }

        /* "Name: value"; no continuation lines or space before the colon */

        colon = ngx_strlchr(line, end, ':');
        if (colon == NULL || colon == line) {
            return NGX_ERROR;
        }

        for (q = line; q < colon; q++) {
            if (*q <= ' ' || *q == 0x7f) {
                return NGX_ERROR;
            }
        }

        v = colon + 1;
        while (v < end && (*v == ' ' || *v == '\t')) {
            v++;
        }

        while (end > v && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }

        /* framing of a body, if the response has one */

        if (!probe->bodyless
            && colon - line == sizeof("Content-Length") - 1
            && ngx_strncasecmp(line, (u_char *) "Content-Length",
                               sizeof("Content-Length") - 1)
               == 0)
        {
            len = ngx_atoof(v, end - v);
            if (len == NGX_ERROR) {
                probe->error = (u_char *) "invalid Content-Length";
                return NGX_ERROR;
            }

            if (probe->content_length >= 0 && probe->content_length != len) {
                probe->error = (u_char *) "conflicting Content-Length";
                return NGX_ERROR;
            }

            probe->content_length = len;
        }

        if (!probe->bodyless
            && colon - line == sizeof("Transfer-Encoding") - 1
            && ngx_strncasecmp(line, (u_char *) "Transfer-Encoding",
                               sizeof("Transfer-Encoding") - 1)
               == 0)
        {
            probe->error = (u_char *) "unsupported Transfer-Encoding";
            return NGX_ERROR;
        }

        if (store) {
            h = ngx_array_push(probe->headers);
            if (h == NULL) {
                return NGX_ABORT;
            }

            h->name.data = line;
            h->name.len = colon - line;
            h->value.data = v;
            h->value.len = end - v;
        }
    }

too_large:

    probe->error = (u_char *) "response header too large";
    return NGX_ERROR;
}


/* The response is complete (or 256k of body): judge it. */

static void
ngx_http_health_check_evaluate(ngx_http_health_check_probe_t *probe)
{
    size_t      len;
    ngx_str_t   body;
    ngx_int_t   rc;

    rc = ngx_http_health_check_parse(probe, 1);

    if (rc == NGX_ABORT) {
        ngx_http_health_check_finish(probe, (u_char *) "out of memory");
        return;
    }

    if (rc == NGX_AGAIN) {
        ngx_http_health_check_finish(probe,
                                     (u_char *) "incomplete response header");
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_http_health_check_finish(probe, probe->error);
        return;
    }

    body.data = probe->buf.start + probe->header_len;
    len = probe->buf.last - body.data;

    if (probe->bodyless) {
        len = 0;

    } else if (probe->content_length >= 0) {

        if ((off_t) len > probe->content_length) {
            len = (size_t) probe->content_length;
        }

        /* reading stops early only once enough of the body is here */

        if ((off_t) len < ngx_min(probe->content_length,
                                  NGX_HTTP_HEALTH_CHECK_BODY_MAX))
        {
            ngx_http_health_check_finish(probe, (u_char *)
                  "connection closed before the response body was complete");
            return;
        }
    }

    body.len = ngx_min(len, NGX_HTTP_HEALTH_CHECK_BODY_MAX);

    ngx_http_health_check_finish(probe,
        ngx_http_health_check_match_response(probe->check->match, probe->pool,
                                             probe->status, probe->headers,
                                             &body));
}


/* Records the result (reason == NULL: passed) and frees the probe. */

static void
ngx_http_health_check_finish(ngx_http_health_check_probe_t *probe,
    u_char *reason)
{
    uint64_t                           bit;
    ngx_uint_t                         i, was, now;
    ngx_http_upstream_rr_peer_t       *peer;
    ngx_http_upstream_rr_peers_t      *peers;
    ngx_http_health_check_t           *check;
    ngx_http_health_check_peer_t      *st;
    ngx_http_health_check_upstream_t  *up;

    check = probe->check;
    up = check->upstream;
    bit = (uint64_t) 1 << check->index;

    if (probe->pc.connection) {
        ngx_close_connection(probe->pc.connection);
        probe->pc.connection = NULL;
    }

    st = up->peers->elts;
    for (i = 0; i < up->peers->nelts; i++) {
        if (ngx_cmp_sockaddr(st[i].sockaddr, st[i].socklen,
                             probe->sockaddr, probe->socklen, 1)
            == NGX_OK)
        {
            break;
        }
    }

    if (i == up->peers->nelts) {
        st = ngx_array_push(up->peers);
        if (st == NULL) {
            goto done;
        }

        st->sockaddr = ngx_alloc(probe->socklen, ngx_cycle->log);
        if (st->sockaddr == NULL) {
            up->peers->nelts--;
            goto done;
        }

        ngx_memcpy(st->sockaddr, probe->sockaddr, probe->socklen);
        st->socklen = probe->socklen;
        st->failing = 0;

    } else {
        st = &st[i];
    }

    was = (st->failing != 0);

    if (reason) {
        st->failing |= bit;

    } else {
        st->failing &= ~bit;
    }

    now = (st->failing != 0);

    for (peers = up->uscf->peer.data; peers; peers = peers->next) {

        ngx_http_upstream_rr_peers_wlock(peers);

        for (peer = peers->peer; peer; peer = peer->next) {
            if (ngx_cmp_sockaddr(peer->sockaddr, peer->socklen,
                                 probe->sockaddr, probe->socklen, 1)
                != NGX_OK
                || peer->down == now
                || ngx_http_health_check_admin_down(up, peer))
            {
                continue;
            }

            /*
             * peers->tries counts the servers that aren't down. The zone
             * module adjusts it by the same rule when "resolve" adds or
             * removes a server, so it has to stay exact.
             */

            peer->down = now;

            if (now) {
                peers->tries--;

            } else {
                peers->tries++;
            }
        }

        ngx_http_upstream_rr_peers_unlock(peers);
    }

    if (now && !was) {
        ngx_log_error(NGX_LOG_WARN, check->timer.log, 0,
                      "peer %V in upstream \"%V\" is unhealthy: %s "
                      "(health_check in %V:%ui)",
                      &probe->name, &up->uscf->host, reason,
                      &check->file, check->line);

    } else if (was && !now) {
        ngx_log_error(NGX_LOG_NOTICE, check->timer.log, 0,
                      "peer %V in upstream \"%V\" is healthy",
                      &probe->name, &up->uscf->host);
    }

done:

    ngx_destroy_pool(probe->pool);

    if (--check->pending == 0 && !ngx_exiting && !ngx_quit && !ngx_terminate)
    {
        ngx_add_timer(&check->timer, check->interval);
    }
}
