/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Probing servers and recording their health, in worker 0.
 *
 * Every interval, a check starts a probe for each server of its group
 * whose last probe is over. A probe connects (with a TLS handshake for
 * https), sends the request, reads the response, and records the result in
 * the server's "down" field. At most a quarter of worker_connections probes
 * have a connection at a time; the others wait.
 *
 * A probe holds a reference to its server, as a request does, so that the
 * server stays in memory if "resolve" removes it meanwhile; the result of
 * such a probe is dropped.
 */


#include "ngx_http_health_check.h"


/* One check of one server. */

typedef struct {
    ngx_rbtree_node_t               node;        /* key: the peer */
    ngx_queue_t                     queue;       /* while waiting */
    ngx_http_hc_t                  *check;
    ngx_http_upstream_rr_peers_t   *peers;
    ngx_http_upstream_rr_peer_t    *peer;
    ngx_pool_t                     *pool;
    ngx_log_t                       log;
    ngx_peer_connection_t           pc;
    size_t                          sent;
    ngx_http_hc_response_t          resp;
    unsigned                        started:1;
} ngx_http_hc_probe_t;


static void ngx_http_hc_tick(ngx_event_t *ev);
static ngx_uint_t ngx_http_hc_probing(ngx_http_hc_t *hc,
    ngx_http_upstream_rr_peer_t *peer);
static void ngx_http_hc_start(void);
static void ngx_http_hc_connect(ngx_http_hc_probe_t *p);
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
static void ngx_http_hc_close(ngx_http_hc_probe_t *p);
static u_char *ngx_http_hc_log_error(ngx_log_t *log, u_char *buf,
    size_t len);


/* worker 0's checks, and its probes waiting for a connection */
static ngx_http_hc_main_conf_t  *ngx_http_hc_running;
static ngx_queue_t               ngx_http_hc_waiting;
static ngx_uint_t                ngx_http_hc_active;
static ngx_uint_t                ngx_http_hc_max_active;
static ngx_uint_t                ngx_http_hc_starting;


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

    ngx_http_hc_running = hmcf;
    ngx_queue_init(&ngx_http_hc_waiting);

    /* the rest is left to client connections */
    ngx_http_hc_max_active = ngx_max(cycle->connection_n / 4, 1);

    hc = hmcf->checks.elts;

    for (i = 0; i < hmcf->checks.nelts; i++) {
        ngx_rbtree_init(&hc[i].probes, &hc[i].sentinel,
                        ngx_rbtree_insert_value);

        hc[i].timer.handler = ngx_http_hc_tick;
        hc[i].timer.data = &hc[i];
        hc[i].timer.log = cycle->log;
        hc[i].timer.cancelable = 1;

        ngx_add_timer(&hc[i].timer, 1);
    }

    return NGX_OK;
}


/*
 * A worker that shuts down doesn't wait for probes: closes them before
 * nginx looks for sockets left open. The servers keep their state.
 */

void
ngx_http_hc_exit_process(ngx_cycle_t *cycle)
{
    ngx_uint_t      i;
    ngx_http_hc_t  *hc;

    if (ngx_http_hc_running == NULL) {
        return;
    }

    hc = ngx_http_hc_running->checks.elts;

    for (i = 0; i < ngx_http_hc_running->checks.nelts; i++) {
        while (hc[i].probes.root != hc[i].probes.sentinel) {
            ngx_http_hc_close((ngx_http_hc_probe_t *) hc[i].probes.root);
        }
    }
}


/*
 * Every interval: a probe for each server of the group, primary or backup,
 * unless it is down by configuration or its last probe isn't over.
 */

static void
ngx_http_hc_tick(ngx_event_t *ev)
{
    ngx_http_hc_t *hc = ev->data;

    ngx_http_hc_probe_t           *p;
    ngx_http_upstream_rr_peer_t   *peer;
    ngx_http_upstream_rr_peers_t  *peers;

    if (ngx_exiting || ngx_quit || ngx_terminate) {
        return;
    }

    for (peers = hc->uscf->peer.data; peers; peers = peers->next) {

        ngx_http_upstream_rr_peers_rlock(peers);

        for (peer = peers->peer; peer; peer = peer->next) {

            if ((peer->down & NGX_HTTP_UPSTREAM_FAILED)
                || ngx_http_hc_probing(hc, peer))
            {
                continue;
            }

            p = ngx_calloc(sizeof(ngx_http_hc_probe_t), ev->log);
            if (p == NULL) {
                continue;
            }

            p->check = hc;
            p->peers = peers;
            p->peer = peer;
            p->node.key = (ngx_rbtree_key_t) peer;

            ngx_http_upstream_rr_peer_lock(peers, peer);
            ngx_http_upstream_rr_peer_ref(peers, peer);
            ngx_http_upstream_rr_peer_unlock(peers, peer);

            ngx_rbtree_insert(&hc->probes, &p->node);
            ngx_queue_insert_tail(&ngx_http_hc_waiting, &p->queue);
        }

        ngx_http_upstream_rr_peers_unlock(peers);
    }

    ngx_add_timer(ev, hc->interval);

    ngx_http_hc_start();
}


/* Whether the check has a probe of the server waiting or in progress. */

static ngx_uint_t
ngx_http_hc_probing(ngx_http_hc_t *hc, ngx_http_upstream_rr_peer_t *peer)
{
    ngx_rbtree_key_t    key;
    ngx_rbtree_node_t  *node;

    key = (ngx_rbtree_key_t) peer;
    node = hc->probes.root;

    while (node != hc->probes.sentinel) {
        if (key == node->key) {
            return 1;
        }

        node = (key < node->key) ? node->left : node->right;
    }

    return 0;
}


/* Starts waiting probes while fewer than max_active have a connection. */

static void
ngx_http_hc_start(void)
{
    ngx_queue_t          *q;
    ngx_http_hc_probe_t  *p;

    /* a probe that ends at once calls this again: the loop goes on */

    if (ngx_http_hc_starting) {
        return;
    }

    ngx_http_hc_starting = 1;

    while (ngx_http_hc_active < ngx_http_hc_max_active
           && !ngx_queue_empty(&ngx_http_hc_waiting)
           && !ngx_exiting)
    {
        q = ngx_queue_head(&ngx_http_hc_waiting);
        ngx_queue_remove(q);

        p = ngx_queue_data(q, ngx_http_hc_probe_t, queue);
        p->started = 1;
        ngx_http_hc_active++;

        ngx_http_hc_connect(p);
    }

    ngx_http_hc_starting = 0;
}


static void
ngx_http_hc_connect(ngx_http_hc_probe_t *p)
{
    ngx_int_t                   rc;
    ngx_connection_t           *c;
    ngx_http_upstream_conf_t   *conf;
    ngx_http_upstream_local_t  *local;

    conf = p->check->conf;

    p->log = *ngx_cycle->log;
    p->log.handler = ngx_http_hc_log_error;
    p->log.data = p;

    p->pool = ngx_create_pool(1024, &p->log);
    if (p->pool == NULL) {
        ngx_http_hc_abort(p, "out of memory");
        return;
    }

    p->pc.sockaddr = p->peer->sockaddr;
    p->pc.socklen = p->peer->socklen;
    p->pc.name = &p->peer->name;
    p->pc.get = ngx_event_get_peer;
    p->pc.log = &p->log;
    p->pc.log_error = NGX_ERROR_INFO;

    /* proxy_bind with variables: no request to evaluate them for */

    local = conf->local;

    if (local && local->value == NULL) {
        p->pc.local = local->addr;
#if (NGX_HAVE_TRANSPARENT_PROXY)
        p->pc.transparent = local->transparent;
#endif
    }

    rc = ngx_event_connect_peer(&p->pc);

    if (rc == NGX_DECLINED) {
        ngx_http_hc_finish(p, "connect() failed");
        return;
    }

    if (rc != NGX_OK && rc != NGX_AGAIN) {
        ngx_http_hc_abort(p, "cannot open a connection");
        return;
    }

    c = p->pc.connection;
    c->data = p;
    c->pool = p->pool;
    c->read->handler = ngx_http_hc_connected;
    c->write->handler = ngx_http_hc_connected;

    /* don't hold up a worker that shuts down */
    c->read->cancelable = 1;
    c->write->cancelable = 1;

    p->log.connection = c->number;

    if (ngx_http_hc_response_init(&p->resp, c, conf->buffer_size,
                                  p->check->match && p->check->match->body)
        != NGX_OK)
    {
        ngx_http_hc_abort(p, "out of memory");
        return;
    }

    /* for https, the handshake must be done within it too */
    ngx_add_timer(c->write, conf->connect_timeout);

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
    ngx_str_t            *req;
    ngx_connection_t     *c;
    ngx_http_hc_probe_t  *p;

    c = wev->data;
    p = c->data;
    req = &p->check->request;

    if (wev->timedout) {
        ngx_http_hc_finish(p, "timed out sending the request");
        return;
    }

    while (p->sent < req->len) {
        n = c->send(c, req->data + p->sent, req->len - p->sent);

        if (n == NGX_ERROR) {
            ngx_http_hc_finish(p, "send() failed");
            return;
        }

        if (n == NGX_AGAIN) {
            ngx_add_timer(wev, p->check->conf->send_timeout);

            if (ngx_handle_write_event(wev, 0) != NGX_OK) {
                ngx_http_hc_abort(p, "cannot handle events");
            }

            return;
        }

        p->sent += n;
    }

    if (wev->timer_set) {
        ngx_del_timer(wev);
    }

    /* no more interest in writing, for poll and select */

    wev->handler = ngx_http_hc_dummy;

    if (ngx_handle_write_event(wev, 0) != NGX_OK) {
        ngx_http_hc_abort(p, "cannot handle events");
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
    ngx_http_hc_start();
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
    ngx_http_hc_start();
}


static void
ngx_http_hc_close(ngx_http_hc_probe_t *p)
{
    ngx_connection_t  *c;

    c = p->pc.connection;

    if (c) {
#if (NGX_HTTP_SSL)
        if (c->ssl) {
            c->ssl->no_wait_shutdown = 1;
            c->ssl->no_send_shutdown = 1;
            (void) ngx_ssl_shutdown(c);
        }
#endif

        ngx_close_connection(c);
    }

    if (p->pool) {
        ngx_destroy_pool(p->pool);
    }

    if (p->started) {
        ngx_http_hc_active--;

    } else {
        ngx_queue_remove(&p->queue);
    }

    ngx_rbtree_delete(&p->check->probes, &p->node);

    /* as ngx_http_upstream_free_round_robin_peer() */

    ngx_http_upstream_rr_peers_rlock(p->peers);
    ngx_http_upstream_rr_peer_lock(p->peers, p->peer);

    if (ngx_http_upstream_rr_peer_unref(p->peers, p->peer) == NGX_OK) {
        ngx_http_upstream_rr_peer_unlock(p->peers, p->peer);
    }

    ngx_http_upstream_rr_peers_unlock(p->peers);

    ngx_free(p);
}


static u_char *
ngx_http_hc_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    ngx_http_hc_probe_t  *p = log->data;

    return ngx_snprintf(buf, len, " while checking peer %V in upstream \"%V\"",
                        &p->peer->name, &p->check->uscf->host);
}
