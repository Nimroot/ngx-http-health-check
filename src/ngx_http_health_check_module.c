/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Active health checks for upstream groups in shared memory: the
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
 *         body ~ "^ok";
 *     }
 *
 *     location / {
 *         proxy_pass http://backend;
 *         health_check interval=5s uri=/health match=ok;
 *     }
 *
 * This file: the directives, and tying each health_check to its location's
 * group, proxy settings and match block.
 */


#include "ngx_http_health_check.h"

#include <ngx_http_proxy_module.h>


#define NGX_HTTP_HC_MAX_CHECKS  64    /* per group: failing bits */


static void *ngx_http_hc_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_hc_health_check(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_http_hc_upstream_t *ngx_http_hc_get_upstream(ngx_conf_t *cf,
    ngx_http_hc_main_conf_t *hmcf, ngx_http_upstream_srv_conf_t *uscf);
static ngx_int_t ngx_http_hc_postconf(ngx_conf_t *cf);
#if (NGX_HTTP_SSL)
static ngx_int_t ngx_http_hc_ssl_conf(ngx_conf_t *cf, ngx_http_hc_t *hc);
#endif


static ngx_command_t  ngx_http_hc_commands[] = {

    { ngx_string("health_check"),
      NGX_HTTP_LOC_CONF|NGX_CONF_ANY,
      ngx_http_hc_health_check,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("match"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
      ngx_http_hc_match_block,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_hc_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_hc_postconf,                  /* postconfiguration */

    ngx_http_hc_create_main_conf,          /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_health_check_module = {
    NGX_MODULE_V1,
    &ngx_http_hc_module_ctx,               /* module context */
    ngx_http_hc_commands,                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_hc_init_process,              /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/* health_check parameters that are recognized but not implemented */
static ngx_str_t  ngx_http_hc_unsupported[] = {
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
ngx_http_hc_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_hc_main_conf_t  *hmcf;

    hmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_hc_main_conf_t));
    if (hmcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&hmcf->matches, cf->pool, 4,
                       sizeof(ngx_http_hc_match_t))
        != NGX_OK
        || ngx_array_init(&hmcf->checks, cf->pool, 4, sizeof(ngx_http_hc_t))
           != NGX_OK
        || ngx_array_init(&hmcf->upstreams, cf->pool, 4,
                          sizeof(ngx_http_hc_upstream_t *))
           != NGX_OK)
    {
        return NULL;
    }

    return hmcf;
}


/* health_check [interval=time] [uri=uri] [match=name]; */

static char *
ngx_http_hc_health_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_hc_main_conf_t *hmcf = conf;

    u_char         *p;
    ngx_str_t      *value, s, *u;
    ngx_int_t       ms;
    ngx_uint_t      i;
    ngx_http_hc_t  *hc;

    hc = ngx_array_push(&hmcf->checks);
    if (hc == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(hc, sizeof(ngx_http_hc_t));

    hc->interval = 5000;
    ngx_str_set(&hc->uri, "/");
    hc->loc_conf = ((ngx_http_conf_ctx_t *) cf->ctx)->loc_conf;
    hc->file = cf->conf_file->file.name;
    hc->line = cf->conf_file->line;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            s.data = value[i].data + 9;
            s.len = value[i].len - 9;

            ms = ngx_parse_time(&s, 0);
            if (ms == NGX_ERROR || ms == 0) {
                goto invalid;
            }

            hc->interval = (ngx_msec_t) ms;
            continue;
        }

        if (ngx_strncmp(value[i].data, "uri=", 4) == 0) {
            s.data = value[i].data + 4;
            s.len = value[i].len - 4;

            /* it goes into the request line as it is */

            if (s.len == 0 || s.data[0] != '/') {
                goto invalid;
            }

            for (p = s.data; p < s.data + s.len; p++) {
                if (*p <= ' ' || *p == 0x7f) {
                    goto invalid;
                }
            }

            hc->uri = s;
            continue;
        }

        if (ngx_strncmp(value[i].data, "match=", 6) == 0
            && value[i].len > 6)
        {
            hc->match_name.data = value[i].data + 6;
            hc->match_name.len = value[i].len - 6;
            continue;
        }

        for (u = ngx_http_hc_unsupported; u->len; u++) {
            if (ngx_strncmp(value[i].data, u->data, u->len) == 0
                && (u->data[u->len - 1] == '=' || value[i].len == u->len))
            {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "\"%V\" is not supported by "
                                   "ngx_http_health_check_module", &value[i]);
                return NGX_CONF_ERROR;
            }
        }

        goto invalid;
    }

    return NGX_CONF_OK;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);
    return NGX_CONF_ERROR;
}


static ngx_http_hc_upstream_t *
ngx_http_hc_get_upstream(ngx_conf_t *cf, ngx_http_hc_main_conf_t *hmcf,
    ngx_http_upstream_srv_conf_t *uscf)
{
    ngx_uint_t                   i;
    ngx_http_upstream_server_t  *server, **sp;
    ngx_http_hc_upstream_t      *up, **upp;

    upp = hmcf->upstreams.elts;
    for (i = 0; i < hmcf->upstreams.nelts; i++) {
        if (upp[i]->uscf == uscf) {
            return upp[i];
        }
    }

    up = ngx_pcalloc(cf->pool, sizeof(ngx_http_hc_upstream_t));
    if (up == NULL) {
        return NULL;
    }

    up->uscf = uscf;

    if (ngx_array_init(&up->down, cf->pool, 2,
                       sizeof(ngx_http_upstream_server_t *))
        != NGX_OK)
    {
        return NULL;
    }

    /* servers marked "down" in the configuration stay down */

    if (uscf->servers) {
        server = uscf->servers->elts;
        for (i = 0; i < uscf->servers->nelts; i++) {
            if (!server[i].down || server[i].host.len) {
                continue;
            }

            sp = ngx_array_push(&up->down);
            if (sp == NULL) {
                return NULL;
            }

            *sp = &server[i];
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
 * Once the http block is parsed and merged: ties each health_check to its
 * location's group, proxy settings and match block, and builds its request.
 */

static ngx_int_t
ngx_http_hc_postconf(ngx_conf_t *cf)
{
    u_char                     *p;
    size_t                      len;
    ngx_uint_t                  i;
    ngx_http_hc_t              *hc;
    ngx_http_hc_upstream_t     *up;
    ngx_http_hc_main_conf_t    *hmcf;
    ngx_http_proxy_loc_conf_t  *plcf;

    hmcf = ngx_http_conf_get_module_main_conf(cf,
                                              ngx_http_health_check_module);
    hc = hmcf->checks.elts;

    for (i = 0; i < hmcf->checks.nelts; i++) {

        plcf = hc[i].loc_conf[ngx_http_proxy_module.ctx_index];

        hc[i].conf = &plcf->upstream;
        hc[i].uscf = plcf->upstream.upstream;

        if (hc[i].uscf == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "\"health_check\" needs \"proxy_pass\" to an "
                          "upstream group (without variables) in the same "
                          "location in %V:%ui", &hc[i].file, hc[i].line);
            return NGX_ERROR;
        }

        if (hc[i].uscf->shm_zone == NULL) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "upstream \"%V\" must reside in shared memory "
                          "(add a \"zone\" directive) for \"health_check\" "
                          "in %V:%ui", &hc[i].uscf->host,
                          &hc[i].file, hc[i].line);
            return NGX_ERROR;
        }

        if (hc[i].match_name.len) {
            hc[i].match = ngx_http_hc_match_find(hmcf, &hc[i].match_name);

            if (hc[i].match == NULL) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                              "match \"%V\" not found in %V:%ui",
                              &hc[i].match_name, &hc[i].file, hc[i].line);
                return NGX_ERROR;
            }
        }

        up = ngx_http_hc_get_upstream(cf, hmcf, hc[i].uscf);
        if (up == NULL) {
            return NGX_ERROR;
        }

        if (up->nchecks == NGX_HTTP_HC_MAX_CHECKS) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "too many health checks for upstream \"%V\" "
                          "in %V:%ui", &hc[i].uscf->host,
                          &hc[i].file, hc[i].line);
            return NGX_ERROR;
        }

        hc[i].upstream = up;
        hc[i].index = up->nchecks++;

        /*
         * Host is $proxy_host. Probes don't speak HTTP/2: with
         * proxy_http_version 2 they use HTTP/1.1.
         */

        len = sizeof("GET  HTTP/1.x" CRLF "Host: " CRLF
                     "Connection: close" CRLF CRLF) - 1
              + hc[i].uri.len + plcf->vars.host_header.len;

        p = ngx_pnalloc(cf->pool, len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        hc[i].request.data = p;
        hc[i].request.len = ngx_sprintf(p, "GET %V HTTP/1.%c" CRLF
                                           "Host: %V" CRLF
                                           "Connection: close" CRLF CRLF,
                                        &hc[i].uri,
                                        plcf->http_version
                                            == NGX_HTTP_VERSION_10
                                        ? '0' : '1',
                                        &plcf->vars.host_header)
                            - p;

#if (NGX_HTTP_SSL)
        if (plcf->ssl && ngx_http_hc_ssl_conf(cf, &hc[i]) != NGX_OK) {
            return NGX_ERROR;
        }
#elif (NGX_COMPAT)
        if (plcf->ssl) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "\"health_check\" over https needs "
                          "ngx_http_health_check_module built with "
                          "--with-http_ssl_module in %V:%ui",
                          &hc[i].file, hc[i].line);
            return NGX_ERROR;
        }
#endif
    }

    return NGX_OK;
}


#if (NGX_HTTP_SSL)

/*
 * proxy_pass https://: probes use the location's SSL context and its
 * proxy_ssl_verify, proxy_ssl_server_name and proxy_ssl_name, whose default
 * is the group's name, as requests do. With no request to evaluate them
 * for, a name or certificate given with variables has no value.
 */

static ngx_int_t
ngx_http_hc_ssl_conf(ngx_conf_t *cf, ngx_http_hc_t *hc)
{
    u_char                    *p, *last;
    ngx_str_t                  name;
    ngx_http_upstream_conf_t  *conf;

    conf = hc->conf;

    if ((conf->ssl_name && conf->ssl_name->lengths)
        || (conf->ssl_certificate
            && conf->ssl_certificate->value.len
            && (conf->ssl_certificate->lengths
                || conf->ssl_certificate_key->lengths)))
    {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "\"health_check\" cannot use \"proxy_ssl_name\", "
                      "\"proxy_ssl_certificate\" or "
                      "\"proxy_ssl_certificate_key\" with variables in %V:%ui",
                      &hc->file, hc->line);
        return NGX_ERROR;
    }

    hc->ssl = conf->ssl;

    /* without a port, as ngx_http_upstream_ssl_name() does */

    name = conf->ssl_name ? conf->ssl_name->value : hc->uscf->host;

    p = name.data;
    last = name.data + name.len;

    if (name.len && *p == '[') {
        p = ngx_strlchr(p, last, ']');

        if (p == NULL) {
            p = name.data;
        }
    }

    p = ngx_strlchr(p, last, ':');

    if (p != NULL) {
        name.len = p - name.data;
    }

    hc->ssl_name = name;

    /* as per RFC 6066, literal IPv4 and IPv6 addresses are not sent */

    if (!conf->ssl_server_name
        || name.len == 0
        || *name.data == '['
        || ngx_inet_addr(name.data, name.len) != INADDR_NONE)
    {
        return NGX_OK;
    }

    hc->ssl_server_name = ngx_pnalloc(cf->pool, name.len + 1);
    if (hc->ssl_server_name == NULL) {
        return NGX_ERROR;
    }

    (void) ngx_cpystrn(hc->ssl_server_name, name.data, name.len + 1);

    return NGX_OK;
}

#endif
