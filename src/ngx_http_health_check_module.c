/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Active health checks for nginx upstream groups, configured with the
 * health_check and match directives.
 *
 *     upstream backend {
 *         zone backend 256k;
 *         server 10.0.0.1:8080;
 *         server 10.0.0.2:8080;
 *     }
 *
 *     match ok {
 *         status 200;
 *         header Content-Type = text/plain;
 *         body ~ "^ok";
 *     }
 *
 *     location / {
 *         proxy_pass http://backend;
 *         health_check interval=5s uri=/health match=ok;
 *     }
 *
 * Supported: the interval=, uri= and match= parameters of health_check, and
 * the status, header and body tests of match. Other parameters and the
 * "require" test are rejected at configuration time.
 *
 * How it works: health_check needs the upstream to have a "zone", so its
 * servers live in shared memory and are seen by every worker. Worker 0 sends
 * "GET <uri> HTTP/1.0" to every server of the group every interval and sets
 * the server's "down" flag from the result, which every load-balancing
 * method already honours; it keeps the group's count of usable servers
 * (peers->tries) in step, as nginx does for "down" servers. Servers marked
 * "down" in the configuration are never checked or brought up. Fails and
 * passes are fixed at 1, servers start healthy, and probes use the
 * location's proxy_connect/send/read timeouts and Host $proxy_host.
 *
 * Source files:
 *
 *     ngx_http_health_check_module.c   directives, configuration, start-up
 *     ngx_http_health_check_match.c    the "match" block and its tests
 *     ngx_http_health_check_probe.c    probing servers, recording health
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#if (nginx_version < 1029004)
#error ngx_http_health_check_module needs nginx 1.29.4 or later
#endif


#include <ngx_http_proxy_module.h>


#if !(NGX_HTTP_UPSTREAM_ZONE)
#error ngx_http_health_check_module needs ngx_http_upstream_zone_module
#endif

#if !(NGX_PCRE)
#error ngx_http_health_check_module needs PCRE
#endif


#include "ngx_http_health_check.h"
#include "ngx_http_health_check_match.h"
#include "ngx_http_health_check_probe.h"


#define NGX_HTTP_HEALTH_CHECK_MAX_CHECKS  64    /* per group: failing bits */


static void *ngx_http_health_check_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_health_check(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_http_health_check_upstream_t *ngx_http_health_check_get_upstream(
    ngx_conf_t *cf, ngx_http_health_check_main_conf_t *hmcf,
    ngx_http_upstream_srv_conf_t *uscf);
static ngx_int_t ngx_http_health_check_postconf(ngx_conf_t *cf);
static ngx_int_t ngx_http_health_check_init_process(ngx_cycle_t *cycle);


static ngx_command_t  ngx_http_health_check_commands[] = {

    { ngx_string("health_check"),
      NGX_HTTP_LOC_CONF|NGX_CONF_ANY,
      ngx_http_health_check,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("match"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
      ngx_http_health_check_match_block,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_health_check_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_health_check_postconf,        /* postconfiguration */

    ngx_http_health_check_create_main_conf, /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_health_check_module = {
    NGX_MODULE_V1,
    &ngx_http_health_check_module_ctx,     /* module context */
    ngx_http_health_check_commands,        /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_health_check_init_process,    /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/* health_check parameters that are recognized but not implemented. */
static ngx_str_t  ngx_http_health_check_unsupported[] = {
    ngx_string("jitter="),
    ngx_string("fails="),
    ngx_string("passes="),
    ngx_string("mandatory"),
    ngx_string("persistent"),
    ngx_string("port="),
    ngx_string("type="),
    ngx_string("grpc_service="),
    ngx_string("grpc_status="),
    ngx_string("keepalive_time="),
    ngx_null_string
};


static void *
ngx_http_health_check_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_health_check_main_conf_t  *hmcf;

    hmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_health_check_main_conf_t));
    if (hmcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&hmcf->matches, cf->pool, 4,
                       sizeof(ngx_http_health_check_match_t))
        != NGX_OK
        || ngx_array_init(&hmcf->checks, cf->pool, 4,
                          sizeof(ngx_http_health_check_t *))
           != NGX_OK
        || ngx_array_init(&hmcf->upstreams, cf->pool, 4,
                          sizeof(ngx_http_health_check_upstream_t *))
           != NGX_OK)
    {
        return NULL;
    }

    return hmcf;
}


/* health_check [interval=time] [uri=uri] [match=name]; */

static char *
ngx_http_health_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_health_check_main_conf_t *hmcf = conf;

    u_char                    *p;
    ngx_str_t                 *value, s, *u;
    ngx_int_t                  ms;
    ngx_uint_t                 i;
    ngx_http_conf_ctx_t       *ctx;
    ngx_http_health_check_t   *check, **cp;

    check = ngx_pcalloc(cf->pool, sizeof(ngx_http_health_check_t));
    if (check == NULL) {
        return NGX_CONF_ERROR;
    }

    check->interval = 5000;
    ngx_str_set(&check->uri, "/");

    ctx = cf->ctx;
    check->loc_conf = ctx->loc_conf;
    check->file = cf->conf_file->file.name;
    check->line = cf->conf_file->line;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            s.len = value[i].len - 9;
            s.data = value[i].data + 9;

            ms = ngx_parse_time(&s, 0);
            if (ms == NGX_ERROR || ms == 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid \"interval\" value \"%V\"",
                                   &value[i]);
                return NGX_CONF_ERROR;
            }

            check->interval = (ngx_msec_t) ms;
            continue;
        }

        if (ngx_strncmp(value[i].data, "uri=", 4) == 0) {
            s.len = value[i].len - 4;
            s.data = value[i].data + 4;

            if (s.len == 0) {
                goto invalid;
            }

            /* the URI goes into the request line verbatim */
            for (p = s.data; p < s.data + s.len; p++) {
                if (*p <= ' ' || *p == 0x7f) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "invalid \"uri\" value \"%V\"",
                                       &value[i]);
                    return NGX_CONF_ERROR;
                }
            }

            check->uri = s;
            continue;
        }

        if (ngx_strncmp(value[i].data, "match=", 6) == 0) {
            s.len = value[i].len - 6;
            s.data = value[i].data + 6;

            if (s.len == 0) {
                goto invalid;
            }

            check->match_name = s;
            continue;
        }

        for (u = ngx_http_health_check_unsupported; u->len; u++) {
            if (value[i].len >= u->len
                && ngx_strncmp(value[i].data, u->data, u->len) == 0
                && (u->data[u->len - 1] == '=' || value[i].len == u->len))
            {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "\"health_check\" parameter \"%V\" is not "
                                   "supported by ngx_http_health_check_module "
                                   "(supported: interval=, uri=, match=)",
                                   &value[i]);
                return NGX_CONF_ERROR;
            }
        }

        goto invalid;
    }

    cp = ngx_array_push(&hmcf->checks);
    if (cp == NULL) {
        return NGX_CONF_ERROR;
    }

    *cp = check;

    return NGX_CONF_OK;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);
    return NGX_CONF_ERROR;
}


static ngx_http_health_check_upstream_t *
ngx_http_health_check_get_upstream(ngx_conf_t *cf,
    ngx_http_health_check_main_conf_t *hmcf, ngx_http_upstream_srv_conf_t *uscf)
{
    ngx_str_t                          *name;
    ngx_uint_t                          i;
    ngx_http_upstream_server_t         *server;
    ngx_http_health_check_upstream_t   *up, **upp;

    upp = hmcf->upstreams.elts;
    for (i = 0; i < hmcf->upstreams.nelts; i++) {
        if (upp[i]->uscf == uscf) {
            return upp[i];
        }
    }

    up = ngx_pcalloc(cf->pool, sizeof(ngx_http_health_check_upstream_t));
    if (up == NULL) {
        return NULL;
    }

    up->uscf = uscf;

    if (ngx_array_init(&up->down, cf->pool, 2, sizeof(ngx_str_t)) != NGX_OK) {
        return NULL;
    }

    /* servers marked "down" in the configuration stay down */

    if (uscf->servers) {
        server = uscf->servers->elts;
        for (i = 0; i < uscf->servers->nelts; i++) {
            if (!server[i].down) {
                continue;
            }

            name = ngx_array_push(&up->down);
            if (name == NULL) {
                return NULL;
            }

            *name = server[i].name;
        }
    }

    upp = ngx_array_push(&hmcf->upstreams);
    if (upp == NULL) {
        return NULL;
    }

    *upp = up;

    return up;
}


/*
 * Runs once the whole http{} block is parsed (and merged): ties each
 * health_check to its location's upstream group and match block.
 */

static ngx_int_t
ngx_http_health_check_postconf(ngx_conf_t *cf)
{
    u_char                              *p;
    size_t                               len;
    ngx_str_t                           *host;
    ngx_uint_t                           i;
    ngx_http_upstream_conf_t            *ucf;
    ngx_http_proxy_loc_conf_t           *plcf;
    ngx_http_upstream_srv_conf_t        *uscf;
    ngx_http_health_check_t             *check, **checks;
    ngx_http_health_check_upstream_t    *up;
    ngx_http_health_check_main_conf_t   *hmcf;

    hmcf = ngx_http_conf_get_module_main_conf(cf,
                                              ngx_http_health_check_module);

    checks = hmcf->checks.elts;

    for (i = 0; i < hmcf->checks.nelts; i++) {
        check = checks[i];

        plcf = check->loc_conf[ngx_http_proxy_module.ctx_index];
        ucf = &plcf->upstream;
        uscf = ucf->upstream;

        if (uscf == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "\"health_check\" needs \"proxy_pass\" to an "
                          "upstream group (without variables) in the same "
                          "location in %V:%ui", &check->file, check->line);
            return NGX_ERROR;
        }

#if (NGX_HTTP_SSL || NGX_COMPAT)
        if (plcf->ssl) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "\"health_check\" over https is not supported by "
                          "ngx_http_health_check_module in %V:%ui",
                          &check->file, check->line);
            return NGX_ERROR;
        }
#endif

        if (uscf->shm_zone == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "upstream \"%V\" must reside in shared memory "
                          "(add a \"zone\" directive) for \"health_check\" "
                          "in %V:%ui", &uscf->host, &check->file, check->line);
            return NGX_ERROR;
        }

        if (check->match_name.len) {
            check->match = ngx_http_health_check_match_find(&hmcf->matches,
                                                           &check->match_name);
            if (check->match == NULL) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                              "match \"%V\" not found in %V:%ui",
                              &check->match_name, &check->file, check->line);
                return NGX_ERROR;
            }
        }

        up = ngx_http_health_check_get_upstream(cf, hmcf, uscf);
        if (up == NULL) {
            return NGX_ERROR;
        }

        if (up->nchecks == NGX_HTTP_HEALTH_CHECK_MAX_CHECKS) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "too many health checks for upstream \"%V\" "
                          "in %V:%ui", &uscf->host, &check->file, check->line);
            return NGX_ERROR;
        }

        check->upstream = up;
        check->index = up->nchecks++;

        check->connect_timeout = ucf->connect_timeout;
        check->send_timeout = ucf->send_timeout;
        check->read_timeout = ucf->read_timeout;

        /* Host is $proxy_host, the upstream group's name */

        host = &uscf->host;

        len = sizeof("GET ") - 1 + check->uri.len
              + sizeof(" HTTP/1.0" CRLF "Host: ") - 1 + host->len
              + sizeof(CRLF "Connection: close" CRLF CRLF) - 1;

        p = ngx_pnalloc(cf->pool, len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        check->request.data = p;
        check->request.len = ngx_sprintf(p, "GET %V HTTP/1.0" CRLF
                                            "Host: %V" CRLF
                                            "Connection: close" CRLF CRLF,
                                         &check->uri, host)
                             - p;
    }

    return NGX_OK;
}


/* Checks run in worker 0 only. */

static ngx_int_t
ngx_http_health_check_init_process(ngx_cycle_t *cycle)
{
    ngx_uint_t                           i;
    ngx_http_health_check_t            **checks;
    ngx_http_health_check_upstream_t   **ups;
    ngx_http_health_check_main_conf_t   *hmcf;

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

    ups = hmcf->upstreams.elts;
    for (i = 0; i < hmcf->upstreams.nelts; i++) {
        ups[i]->peers = ngx_array_create(cycle->pool, 4,
                                         sizeof(ngx_http_health_check_peer_t));
        if (ups[i]->peers == NULL) {
            return NGX_ERROR;
        }
    }

    checks = hmcf->checks.elts;
    for (i = 0; i < hmcf->checks.nelts; i++) {
        checks[i]->timer.handler = ngx_http_health_check_run;
        checks[i]->timer.data = checks[i];
        checks[i]->timer.log = cycle->log;
        checks[i]->timer.cancelable = 1;

        ngx_add_timer(&checks[i]->timer, 1);
    }

    return NGX_OK;
}
