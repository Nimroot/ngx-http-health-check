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


/* One check of one server, in its own pool. */

typedef struct {
    ngx_queue_t                     queue;       /* in a round, waiting */
    ngx_http_hc_t                  *check;
    ngx_http_upstream_rr_peers_t   *peers;
    ngx_http_upstream_rr_peer_t    *peer;
    ngx_pool_t                     *pool;
    ngx_peer_connection_t           pc;
    u_char                         *sent;
    ngx_http_hc_response_t          resp;
    unsigned                        started:1;
} ngx_http_hc_probe_t;


/*
 * Probes wait in a queue and at most max_active of them have a connection
 * at a time, so that a round doesn't take every connection of worker 0.
 */
static ngx_queue_t  ngx_http_hc_waiting;
static ngx_uint_t   ngx_http_hc_active;
static ngx_uint_t   ngx_http_hc_max_active;
static ngx_uint_t   ngx_http_hc_starting;


static void ngx_http_hc_run(ngx_event_t *ev);
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
static void ngx_http_hc_finish(ngx_http_hc_probe_t *p, const char *reason);
static void ngx_http_hc_abort(ngx_http_hc_probe_t *p, const char *reason);
static void ngx_http_hc_close(ngx_http_hc_probe_t *probe);


/* Checks run in worker 0 only. */

ngx_int_t
ngx_http_hc_init_process(ngx_cycle_t *cycle)
{
    ngx_uint_t                i;
    ngx_http_hc_t            *hc;
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


/* Timer: probe every server of the group, primary and backup. */

static void
ngx_http_hc_run(ngx_event_t *ev)
{
    ngx_pool_t                    *pool;
    ngx_queue_t                    list;
    ngx_http_upstream_rr_peer_t   *peer;
    ngx_http_upstream_rr_peers_t  *peers;
    ngx_http_hc_t                 *check;
    ngx_http_hc_probe_t           *probe;

    check = ev->data;

    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    ngx_queue_init(&list);

    for (peers = check->uscf->peer.data; peers; peers = peers->next) {

        ngx_http_upstream_rr_peers_rlock(peers);

        for (peer = peers->peer; peer; peer = peer->next) {

            if (peer->down & NGX_HTTP_HC_CONF_DOWN) {
                continue;
            }

            pool = ngx_create_pool(1024, ev->log);
            if (pool == NULL) {
                continue;
            }

            probe = ngx_pcalloc(pool, sizeof(ngx_http_hc_probe_t));
            if (probe == NULL) {
                ngx_destroy_pool(pool);
                continue;
            }

            probe->pool = pool;
            probe->check = check;
            probe->peers = peers;
            probe->peer = peer;

            ngx_http_upstream_rr_peer_lock(peers, peer);
            ngx_http_upstream_rr_peer_ref(peers, peer);
            ngx_http_upstream_rr_peer_unlock(peers, peer);

            ngx_queue_insert_tail(&list, &probe->queue);
            check->pending++;
        }

        ngx_http_upstream_rr_peers_unlock(peers);
    }

    if (ngx_queue_empty(&list)) {
        ngx_add_timer(ev, check->interval);
        return;
    }

    /* the next round is scheduled when the last probe finishes */

    ngx_queue_add(&ngx_http_hc_waiting, &list);

    ngx_http_hc_start();
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

    pc->sockaddr = probe->peer->sockaddr;
    pc->socklen = probe->peer->socklen;
    pc->name = &probe->peer->name;
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


/*
 * The probe went through: records the result in the server, reason NULL
 * if it passed, logs a change, and frees the probe.
 */

static void
ngx_http_hc_finish(ngx_http_hc_probe_t *p, const char *reason)
{
    ngx_uint_t                     was, now;
    ngx_http_hc_t                 *hc;
    ngx_http_upstream_rr_peer_t   *peer;
    ngx_http_upstream_rr_peers_t  *peers;

    hc = p->check;
    peer = p->peer;
    peers = p->peers;

    ngx_http_upstream_rr_peers_wlock(peers);

    was = peer->down;

    /* not if "resolve" removed the server meanwhile */

    if (!peer->zombie) {
        peer->down = reason ? (was | hc->bit) : (was & ~hc->bit);

        /* the servers that aren't down, as nginx counts them */

        if (was && !peer->down) {
            peers->tries++;

        } else if (!was && peer->down) {
            peers->tries--;
        }
    }

    now = peer->down;

    ngx_http_upstream_rr_peers_unlock(peers);

    if (!((was ^ now) & hc->bit)) {
        /* no change */

    } else if (now & hc->bit) {
        ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                      "peer %V in upstream \"%V\" is unhealthy: %s "
                      "(health_check in %V:%ui)",
                      &peer->name, &hc->uscf->host, reason,
                      &hc->file, hc->line);

    } else if (now & NGX_HTTP_HC_MASK) {
        ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                      "peer %V in upstream \"%V\" passes health_check "
                      "in %V:%ui, but fails another",
                      &peer->name, &hc->uscf->host, &hc->file, hc->line);

    } else {
        ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                      "peer %V in upstream \"%V\" is healthy",
                      &peer->name, &hc->uscf->host);
    }

    ngx_http_hc_close(p);
}


/*
 * The probe couldn't be carried out for a reason of worker 0's own, such as
 * no free connection or no memory: that says nothing about the server, whose
 * state is left as it was.
 */

static void
ngx_http_hc_abort(ngx_http_hc_probe_t *p, const char *reason)
{
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                  "peer %V in upstream \"%V\" not checked, state unchanged: "
                  "%s (health_check in %V:%ui)",
                  &p->peer->name, &p->check->uscf->host, reason,
                  &p->check->file, p->check->line);

    ngx_http_hc_close(p);
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

    /* as ngx_http_upstream_free_round_robin_peer() */

    ngx_http_upstream_rr_peers_rlock(probe->peers);
    ngx_http_upstream_rr_peer_lock(probe->peers, probe->peer);

    if (ngx_http_upstream_rr_peer_unref(probe->peers, probe->peer) == NGX_OK) {
        ngx_http_upstream_rr_peer_unlock(probe->peers, probe->peer);
    }

    ngx_http_upstream_rr_peers_unlock(probe->peers);

    ngx_destroy_pool(probe->pool);

    if (--check->pending == 0 && !ngx_exiting && !ngx_quit && !ngx_terminate)
    {
        ngx_add_timer(&check->timer, check->interval);
    }

    ngx_http_hc_start();
}
