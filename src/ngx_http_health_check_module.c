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
static ngx_int_t ngx_http_hc_ssl_conf(ngx_conf_t *cf, ngx_http_hc_t *check,
    ngx_http_proxy_loc_conf_t *plcf);
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


/* health_check parameters that are recognized but not implemented. */
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
        || ngx_array_init(&hmcf->checks, cf->pool, 4,
                          sizeof(ngx_http_hc_t *))
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

    u_char               *p;
    ngx_str_t            *value, s, *u;
    ngx_int_t             ms;
    ngx_uint_t            i;
    ngx_http_conf_ctx_t  *ctx;
    ngx_http_hc_t        *check, **cp;

    check = ngx_pcalloc(cf->pool, sizeof(ngx_http_hc_t));
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

        for (u = ngx_http_hc_unsupported; u->len; u++) {
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
 * Runs once the whole http{} block is parsed (and merged): ties each
 * health_check to its location's upstream group and match block.
 */

static ngx_int_t
ngx_http_hc_postconf(ngx_conf_t *cf)
{
    u_char                        *p;
    size_t                         len;
    ngx_str_t                     *host;
    ngx_uint_t                     i;
    ngx_http_upstream_conf_t      *ucf;
    ngx_http_proxy_loc_conf_t     *plcf;
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_http_hc_t                 *check, **checks;
    ngx_http_hc_upstream_t        *up;
    ngx_http_hc_main_conf_t       *hmcf;

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

#if (NGX_HTTP_SSL)
        if (plcf->ssl
            && ngx_http_hc_ssl_conf(cf, check, plcf) != NGX_OK)
        {
            return NGX_ERROR;
        }
#elif (NGX_COMPAT)
        if (plcf->ssl) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "\"health_check\" over https needs "
                          "ngx_http_health_check_module built with "
                          "--with-http_ssl_module in %V:%ui",
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
            check->match = ngx_http_hc_match_find(&hmcf->matches,
                                                  &check->match_name);
            if (check->match == NULL) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                              "match \"%V\" not found in %V:%ui",
                              &check->match_name, &check->file, check->line);
                return NGX_ERROR;
            }
        }

        up = ngx_http_hc_get_upstream(cf, hmcf, uscf);
        if (up == NULL) {
            return NGX_ERROR;
        }

        if (up->nchecks == NGX_HTTP_HC_MAX_CHECKS) {
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

        /*
         * Host is $proxy_host, the upstream group's name. HTTP/2 isn't
         * spoken by probes: with proxy_http_version 2 they use HTTP/1.1.
         */

        host = &uscf->host;
        check->http11 = (plcf->http_version >= NGX_HTTP_VERSION_11);

        len = sizeof("GET ") - 1 + check->uri.len
              + sizeof(" HTTP/1.x" CRLF "Host: ") - 1 + host->len
              + sizeof(CRLF "Connection: close" CRLF CRLF) - 1;

        p = ngx_pnalloc(cf->pool, len);
        if (p == NULL) {
            return NGX_ERROR;
        }

        check->request.data = p;
        check->request.len = ngx_sprintf(p, "GET %V HTTP/1.%c" CRLF
                                            "Host: %V" CRLF
                                            "Connection: close" CRLF CRLF,
                                         &check->uri,
                                         check->http11 ? '1' : '0', host)
                             - p;
    }

    return NGX_OK;
}


#if (NGX_HTTP_SSL)

/*
 * proxy_pass https://: probes use the location's SSL context, so its
 * protocols, ciphers, client certificate and trusted certificates, and
 * proxy_ssl_verify, proxy_ssl_server_name and proxy_ssl_name, as requests
 * do. The name defaults to the group's name, as $proxy_host does. A probe
 * isn't a request, so a name or certificate given with variables has no
 * value for it.
 */

static ngx_int_t
ngx_http_hc_ssl_conf(ngx_conf_t *cf, ngx_http_hc_t *check,
    ngx_http_proxy_loc_conf_t *plcf)
{
    u_char                    *p, *last;
    ngx_str_t                  name;
    ngx_http_upstream_conf_t  *ucf;

    ucf = &plcf->upstream;

    if (ucf->ssl == NULL || ucf->ssl->ctx == NULL) {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "no SSL context for \"health_check\" in %V:%ui",
                      &check->file, check->line);
        return NGX_ERROR;
    }

    if (ucf->ssl_name && ucf->ssl_name->lengths) {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "\"health_check\" cannot use \"proxy_ssl_name\" "
                      "with variables in %V:%ui", &check->file, check->line);
        return NGX_ERROR;
    }

    if (ucf->ssl_certificate
        && ucf->ssl_certificate->value.len
        && (ucf->ssl_certificate->lengths
            || (ucf->ssl_certificate_key
                && ucf->ssl_certificate_key->lengths)))
    {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "\"health_check\" cannot use \"proxy_ssl_certificate\" "
                      "or \"proxy_ssl_certificate_key\" with variables "
                      "in %V:%ui", &check->file, check->line);
        return NGX_ERROR;
    }

    name = ucf->ssl_name ? ucf->ssl_name->value : ucf->upstream->host;

    /* without a port, as ngx_http_upstream_ssl_name() does */

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

    check->ssl = ucf->ssl;
    check->ssl_name = name;
    check->ssl_verify = ucf->ssl_verify;

    /* as per RFC 6066, literal IPv4 and IPv6 addresses are not sent */

    if (!ucf->ssl_server_name
        || name.len == 0
        || *name.data == '['
        || ngx_inet_addr(name.data, name.len) != INADDR_NONE)
    {
        return NGX_OK;
    }

    p = ngx_pnalloc(cf->pool, name.len + 1);
    if (p == NULL) {
        return NGX_ERROR;
    }

    (void) ngx_cpystrn(p, name.data, name.len + 1);

    check->ssl_server_name = p;

    return NGX_OK;
}

#endif
