/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Probing the servers of a group and recording their health: worker 0
 * connects to each server (and does the SSL handshake for https), sends the
 * check request, reads the response, has it judged by the match tests, and
 * sets the server's "down" flag in shared memory.
 */


#include "ngx_http_health_check.h"


typedef struct ngx_http_hc_probe_s  ngx_http_hc_probe_t;

/* One check of one server, in its own pool. */
struct ngx_http_hc_probe_s {
    ngx_http_hc_t                       *check;
    ngx_pool_t                         *pool;
    ngx_peer_connection_t               pc;
    struct sockaddr                    *sockaddr;
    socklen_t                           socklen;
    ngx_str_t                           name;
    u_char                             *sent;
    ngx_http_hc_response_t              resp;

    /* in the round's list, then in the waiting queue */
    ngx_queue_t                         queue;

    unsigned                            started:1;
};


/*
 * Probes wait in a queue and at most max_active of them have a connection
 * at a time, so that a round doesn't take every connection of worker 0.
 */
static ngx_queue_t  ngx_http_hc_waiting;
static ngx_uint_t   ngx_http_hc_active;
static ngx_uint_t   ngx_http_hc_max_active;
static ngx_uint_t   ngx_http_hc_starting;


static void ngx_http_hc_run(ngx_event_t *ev);
static void ngx_http_hc_prune(ngx_http_hc_upstream_t *up, ngx_queue_t *list);
static void ngx_http_hc_start(void);
static void ngx_http_hc_connect(ngx_http_hc_probe_t *probe);
static void ngx_http_hc_connected(ngx_event_t *ev);
#if (NGX_HTTP_SSL)
static void ngx_http_hc_ssl_handshake(ngx_http_hc_probe_t *p,
    ngx_connection_t *c);
static void ngx_http_hc_ssl_handshaked(ngx_connection_t *c);
#endif
static void ngx_http_hc_send(ngx_event_t *wev);
static void ngx_http_hc_recv(ngx_event_t *rev);
static void ngx_http_hc_dummy(ngx_event_t *ev);
static void ngx_http_hc_finish(ngx_http_hc_probe_t *probe,
    const char *reason);
static void ngx_http_hc_abort(ngx_http_hc_probe_t *probe,
    const char *reason);
static void ngx_http_hc_close(ngx_http_hc_probe_t *probe);
static ngx_uint_t ngx_http_hc_admin_down(ngx_http_hc_upstream_t *up,
    ngx_http_upstream_rr_peer_t *peer);


/* Checks run in worker 0 only. */

ngx_int_t
ngx_http_hc_init_process(ngx_cycle_t *cycle)
{
    ngx_uint_t                i;
    ngx_http_hc_t            *hc;
    ngx_http_hc_upstream_t  **ups;
    ngx_http_hc_main_conf_t  *hmcf;

    if ((ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE)
        || ngx_worker != 0)
    {
        return NGX_OK;
    }

    hmcf = ngx_http_cycle_get_module_main_conf(cycle,
                                               ngx_http_health_check_module);
    if (hmcf == NULL) {
        return NGX_OK;
    }

    ngx_queue_init(&ngx_http_hc_waiting);

    /* the rest is left to client connections */
    ngx_http_hc_max_active = ngx_max(cycle->connection_n / 4, 1);

    ups = hmcf->upstreams.elts;
    for (i = 0; i < hmcf->upstreams.nelts; i++) {
        ups[i]->peers = ngx_array_create(cycle->pool, 4,
                                         sizeof(ngx_http_hc_peer_t));
        if (ups[i]->peers == NULL) {
            return NGX_ERROR;
        }
    }

    hc = hmcf->checks.elts;

    for (i = 0; i < hmcf->checks.nelts; i++) {
        hc[i].timer.handler = ngx_http_hc_run;
        hc[i].timer.data = &hc[i];
        hc[i].timer.log = cycle->log;
        hc[i].timer.cancelable = 1;

        ngx_add_timer(&hc[i].timer, 1);
    }

    return NGX_OK;
}


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
ngx_http_hc_admin_down(ngx_http_hc_upstream_t *up,
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

static void
ngx_http_hc_run(ngx_event_t *ev)
{
    ngx_uint_t                     complete;
    ngx_pool_t                    *pool;
    ngx_queue_t                    list;
    ngx_http_upstream_rr_peer_t   *peer;
    ngx_http_upstream_rr_peers_t  *peers;
    ngx_http_hc_t                 *check;
    ngx_http_hc_probe_t           *probe;
    ngx_http_hc_upstream_t        *up;

    check = ev->data;
    up = check->upstream;

    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    ngx_queue_init(&list);
    complete = 1;

    for (peers = up->uscf->peer.data; peers; peers = peers->next) {

        ngx_http_upstream_rr_peers_rlock(peers);

        for (peer = peers->peer; peer; peer = peer->next) {

            if (ngx_http_hc_admin_down(up, peer)) {
                continue;
            }

            pool = ngx_create_pool(1024, ev->log);
            if (pool == NULL) {
                complete = 0;
                continue;
            }

            probe = ngx_pcalloc(pool, sizeof(ngx_http_hc_probe_t));
            if (probe == NULL) {
                ngx_destroy_pool(pool);
                complete = 0;
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
                complete = 0;
                continue;
            }

            ngx_memcpy(probe->sockaddr, peer->sockaddr, peer->socklen);
            ngx_memcpy(probe->name.data, peer->name.data, peer->name.len);

            ngx_queue_insert_tail(&list, &probe->queue);
            check->pending++;
        }

        ngx_http_upstream_rr_peers_unlock(peers);
    }

    /*
     * A server left out of an incomplete list may still be in the group:
     * its state is kept until a round lists every server.
     */

    if (complete) {
        ngx_http_hc_prune(up, &list);
    }

    if (ngx_queue_empty(&list)) {
        ngx_add_timer(ev, check->interval);
        return;
    }

    /* the next round is scheduled when the last probe finishes */

    ngx_queue_add(&ngx_http_hc_waiting, &list);

    ngx_http_hc_start();
}


/*
 * Forgets the state of addresses that are no longer in the group, such as
 * servers removed by "resolve". Otherwise the list would only ever grow.
 */

static void
ngx_http_hc_prune(ngx_http_hc_upstream_t *up, ngx_queue_t *list)
{
    ngx_uint_t            i;
    ngx_queue_t          *q;
    ngx_http_hc_peer_t   *st;
    ngx_http_hc_probe_t  *probe;

    st = up->peers->elts;
    i = 0;

    while (i < up->peers->nelts) {

        for (q = ngx_queue_head(list);
             q != ngx_queue_sentinel(list);
             q = ngx_queue_next(q))
        {
            probe = ngx_queue_data(q, ngx_http_hc_probe_t, queue);

            if (ngx_cmp_sockaddr(st[i].sockaddr, st[i].socklen,
                                 probe->sockaddr, probe->socklen, 1)
                == NGX_OK)
            {
                break;
            }
        }

        if (q != ngx_queue_sentinel(list)) {
            i++;
            continue;
        }

        ngx_free(st[i].sockaddr);
        st[i] = st[--up->peers->nelts];
    }
}


/* Starts waiting probes while fewer than max_active are running. */

static void
ngx_http_hc_start(void)
{
    ngx_queue_t          *q;
    ngx_http_hc_probe_t  *probe;

    /* a probe that ends at once calls this again: the loop goes on */

    if (ngx_http_hc_starting) {
        return;
    }

    ngx_http_hc_starting = 1;

    while (ngx_http_hc_active < ngx_http_hc_max_active
           && !ngx_queue_empty(&ngx_http_hc_waiting)
           && !ngx_exiting && !ngx_quit && !ngx_terminate)
    {
        q = ngx_queue_head(&ngx_http_hc_waiting);
        ngx_queue_remove(q);

        probe = ngx_queue_data(q, ngx_http_hc_probe_t, queue);

        probe->started = 1;
        ngx_http_hc_active++;

        ngx_http_hc_connect(probe);
    }

    ngx_http_hc_starting = 0;
}


static void
ngx_http_hc_connect(ngx_http_hc_probe_t *probe)
{
    ngx_int_t               rc;
    ngx_connection_t       *c;
    ngx_peer_connection_t  *pc;
    ngx_http_hc_t          *check;

    check = probe->check;
    pc = &probe->pc;

    pc->sockaddr = probe->sockaddr;
    pc->socklen = probe->socklen;
    pc->name = &probe->name;
    pc->get = ngx_event_get_peer;
    pc->log = check->timer.log;
    pc->log_error = NGX_ERROR_INFO;

    rc = ngx_event_connect_peer(pc);

    /* NGX_DECLINED: connect() failed; NGX_ERROR: no socket or connection */

    if (rc == NGX_DECLINED) {
        ngx_http_hc_finish(probe, "connect() failed");
        return;
    }

    if (rc != NGX_OK && rc != NGX_AGAIN) {
        ngx_http_hc_abort(probe, "cannot open a connection");
        return;
    }

    c = pc->connection;
    c->data = probe;
    c->pool = probe->pool;
    c->read->handler = ngx_http_hc_connected;
    c->write->handler = ngx_http_hc_connected;

    /* don't hold up a worker that is shutting down */
    c->read->cancelable = 1;
    c->write->cancelable = 1;

    probe->sent = check->request.data;

    if (ngx_http_hc_response_init(&probe->resp, c, check->conf->buffer_size,
                                  check->match && check->match->body)
        != NGX_OK)
    {
        ngx_http_hc_abort(probe, "out of memory");
        return;
    }

    /* for https, the SSL handshake must be done within it too */
    ngx_add_timer(c->write, check->conf->connect_timeout);

    if (rc == NGX_OK) {
        ngx_http_hc_connected(c->write);
    }
}


/* Connected, or not: either event may tell first. */

static void
ngx_http_hc_connected(ngx_event_t *ev)
{
    int                   err;
    socklen_t             len;
    ngx_connection_t     *c;
    ngx_http_hc_probe_t  *p;

    c = ev->data;
    p = c->data;

    if (ev->timedout) {
        ngx_http_hc_finish(p, "timed out connecting");
        return;
    }

    /* as ngx_http_upstream_test_connect() */

#if (NGX_HAVE_KQUEUE)
    if (ngx_event_flags & NGX_USE_KQUEUE_EVENT) {
        err = (c->write->pending_eof || c->read->pending_eof);

    } else
#endif
    {
        err = 0;
        len = sizeof(int);

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
            == -1)
        {
            err = ngx_socket_errno;
        }

        if (err) {
            (void) ngx_connection_error(c, err, "connect() failed");
        }
    }

    if (err) {
        ngx_http_hc_finish(p, "connect() failed");
        return;
    }

    c->read->handler = ngx_http_hc_dummy;
    c->write->handler = ngx_http_hc_send;

#if (NGX_HTTP_SSL)
    if (p->check->ssl) {
        ngx_http_hc_ssl_handshake(p, c);
        return;
    }
#endif

    ngx_http_hc_send(c->write);
}


#if (NGX_HTTP_SSL)

static void
ngx_http_hc_ssl_handshake(ngx_http_hc_probe_t *p, ngx_connection_t *c)
{
    u_char  *name;

    if (ngx_ssl_create_connection(p->check->ssl, c, NGX_SSL_CLIENT)
        != NGX_OK)
    {
        ngx_http_hc_abort(p, "cannot create an SSL connection");
        return;
    }

    name = p->check->ssl_server_name;

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    if (name && SSL_set_tlsext_host_name(c->ssl->connection, (char *) name)
                == 0)
    {
        ngx_ssl_error(NGX_LOG_ERR, c->log, 0,
                      "SSL_set_tlsext_host_name(\"%s\") failed", name);
        ngx_http_hc_abort(p, "cannot set the SSL server name");
        return;
    }
#endif

    if (ngx_ssl_handshake(c) == NGX_AGAIN) {
        c->ssl->handler = ngx_http_hc_ssl_handshaked;
        return;
    }

    ngx_http_hc_ssl_handshaked(c);
}


/* The handshake is over, or failed: checks the certificate, then sends. */

static void
ngx_http_hc_ssl_handshaked(ngx_connection_t *c)
{
    long                  rc;
    u_char                reason[NGX_MAX_ERROR_STR], *last;
    ngx_http_hc_t        *hc;
    ngx_http_hc_probe_t  *p;

    p = c->data;
    hc = p->check;

    if (!c->ssl->handshaked) {
        ngx_http_hc_finish(p, c->write->timedout ? "timed out in SSL handshake"
                                                 : "SSL handshake failed");
        return;
    }

    if (hc->conf->ssl_verify) {
        rc = SSL_get_verify_result(c->ssl->connection);

        if (rc != X509_V_OK) {
            last = ngx_snprintf(reason, sizeof(reason) - 1,
                                "SSL certificate verify error: (%l:%s)",
                                rc, X509_verify_cert_error_string(rc));
            *last = '\0';

            ngx_http_hc_finish(p, (char *) reason);
            return;
        }

        if (ngx_ssl_check_host(c, &hc->ssl_name) != NGX_OK) {
            last = ngx_snprintf(reason, sizeof(reason) - 1,
                                "SSL certificate does not match \"%V\"",
                                &hc->ssl_name);
            *last = '\0';

            ngx_http_hc_finish(p, (char *) reason);
            return;
        }
    }

    c->read->handler = ngx_http_hc_dummy;
    c->write->handler = ngx_http_hc_send;

    ngx_http_hc_send(c->write);
}

#endif


/*
 * Sends the request. As for requests, the send timeout applies while the
 * socket is full, the read timeout between two reads.
 */

static void
ngx_http_hc_send(ngx_event_t *wev)
{
    ssize_t               n;
    u_char               *last;
    ngx_connection_t     *c;
    ngx_http_hc_t        *check;
    ngx_http_hc_probe_t  *probe;

    c = wev->data;
    probe = c->data;
    check = probe->check;

    if (wev->timedout) {
        ngx_http_hc_finish(probe, "timed out sending the request");
        return;
    }

    last = check->request.data + check->request.len;

    while (probe->sent < last) {
        n = c->send(c, probe->sent, last - probe->sent);

        if (n == NGX_ERROR) {
            ngx_http_hc_finish(probe, "send() failed");
            return;
        }

        if (n == NGX_AGAIN) {
            ngx_add_timer(wev, check->conf->send_timeout);

            if (ngx_handle_write_event(wev, 0) != NGX_OK) {
                ngx_http_hc_abort(probe, "cannot handle events");
            }

            return;
        }

        probe->sent += n;
    }

    if (wev->timer_set) {
        ngx_del_timer(wev);
    }

    /* no more interest in writing, for poll and select */

    wev->handler = ngx_http_hc_dummy;

    if (ngx_handle_write_event(wev, 0) != NGX_OK) {
        ngx_http_hc_abort(probe, "cannot handle events");
        return;
    }

    c->read->handler = ngx_http_hc_recv;

    ngx_http_hc_recv(c->read);
}


static void
ngx_http_hc_recv(ngx_event_t *rev)
{
    u_char                reason[NGX_MAX_ERROR_STR];
    ngx_int_t             rc;
    ngx_connection_t     *c;
    ngx_http_hc_probe_t  *p;

    c = rev->data;
    p = c->data;

    if (rev->timedout) {
        ngx_http_hc_finish(p, "timed out reading the response");
        return;
    }

    rc = ngx_http_hc_response_recv(&p->resp, c);

    if (rc == NGX_AGAIN) {
        ngx_add_timer(rev, p->check->conf->read_timeout);

        if (ngx_handle_read_event(rev, 0) != NGX_OK) {
            ngx_http_hc_abort(p, "cannot handle events");
        }

        return;
    }

    if (rc == NGX_DECLINED) {
        ngx_http_hc_finish(p, p->resp.error);
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_http_hc_abort(p, "out of memory");
        return;
    }

    ngx_http_hc_finish(p, (char *) ngx_http_hc_match(p->check->match,
                                                     &p->resp, reason,
                                                     sizeof(reason)));
}


static void
ngx_http_hc_dummy(ngx_event_t *ev)
{
}


/* Records the result (reason == NULL: passed) and frees the probe. */

static void
ngx_http_hc_finish(ngx_http_hc_probe_t *probe, const char *reason)
{
    uint64_t                       bit;
    ngx_uint_t                     i, was, now;
    ngx_http_upstream_rr_peer_t   *peer;
    ngx_http_upstream_rr_peers_t  *peers;
    ngx_http_hc_t                 *check;
    ngx_http_hc_peer_t            *st;
    ngx_http_hc_upstream_t        *up;

    check = probe->check;
    up = check->upstream;
    bit = (uint64_t) 1 << check->index;

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
                || ngx_http_hc_admin_down(up, peer))
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

    ngx_http_hc_close(probe);
}


/*
 * The probe couldn't be carried out for a reason of worker 0's own, such as
 * no free connection or no memory: that says nothing about the server, whose
 * state is left as it was.
 */

static void
ngx_http_hc_abort(ngx_http_hc_probe_t *probe, const char *reason)
{
    ngx_http_hc_t  *check;

    check = probe->check;

    ngx_log_error(NGX_LOG_ERR, check->timer.log, 0,
                  "peer %V in upstream \"%V\" not checked, state unchanged: "
                  "%s (health_check in %V:%ui)",
                  &probe->name, &check->upstream->uscf->host, reason,
                  &check->file, check->line);

    ngx_http_hc_close(probe);
}


/* Frees the probe, starts waiting ones, and schedules the next round. */

static void
ngx_http_hc_close(ngx_http_hc_probe_t *probe)
{
    ngx_http_hc_t  *check;

    check = probe->check;

    if (probe->pc.connection) {

#if (NGX_HTTP_SSL)
        if (probe->pc.connection->ssl) {
            /* nothing more to say to the server, or to hear from it */
            probe->pc.connection->ssl->no_wait_shutdown = 1;
            probe->pc.connection->ssl->no_send_shutdown = 1;

            (void) ngx_ssl_shutdown(probe->pc.connection);
        }
#endif

        ngx_close_connection(probe->pc.connection);
        probe->pc.connection = NULL;
    }

    if (probe->started) {
        ngx_http_hc_active--;
    }

    ngx_destroy_pool(probe->pool);

    if (--check->pending == 0 && !ngx_exiting && !ngx_quit && !ngx_terminate)
    {
        ngx_add_timer(&check->timer, check->interval);
    }

    ngx_http_hc_start();
}
