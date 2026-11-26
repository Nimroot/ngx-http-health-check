/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Types shared by the module's source files:
 *
 *     ngx_http_health_check_module.c     directives, configuration
 *     ngx_http_health_check_match.c      match blocks and their tests
 *     ngx_http_health_check_probe.c      probing servers, recording health
 */


#ifndef _NGX_HTTP_HEALTH_CHECK_H_INCLUDED_
#define _NGX_HTTP_HEALTH_CHECK_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#if (nginx_version < 1029004)
#error ngx_http_health_check_module needs nginx 1.29.4 or later
#endif

#if !(NGX_HTTP_UPSTREAM_ZONE)
#error ngx_http_health_check_module needs ngx_http_upstream_zone_module
#endif

#if !(NGX_PCRE)
#error ngx_http_health_check_module needs PCRE
#endif


enum {
    NGX_HTTP_HC_STATUS = 0,
    NGX_HTTP_HC_HEADER,
    NGX_HTTP_HC_BODY
};


enum {
    NGX_HTTP_HC_HEADER_PRESENT = 0,             /* header Name;          */
    NGX_HTTP_HC_HEADER_ABSENT,                  /* header ! Name;        */
    NGX_HTTP_HC_HEADER_EQ,                      /* header Name = value;  */
    NGX_HTTP_HC_HEADER_NE,                      /* header Name != value; */
    NGX_HTTP_HC_HEADER_RE,                      /* header Name ~ regex;  */
    NGX_HTTP_HC_HEADER_NRE                      /* header Name !~ regex; */
};


typedef struct {
    ngx_uint_t                          low;
    ngx_uint_t                          high;
} ngx_http_hc_range_t;


typedef struct {
    ngx_uint_t                          type;
    ngx_uint_t                          op;        /* header tests */
    ngx_uint_t                          negate;    /* status and body tests */
    /* ngx_http_hc_range_t */
    ngx_array_t                        *ranges;
    ngx_str_t                           name;
    ngx_str_t                           value;
    ngx_regex_t                        *regex;
    ngx_str_t                           text;      /* the test as written */
} ngx_http_hc_test_t;


typedef struct {
    ngx_str_t                           name;
    /* ngx_http_hc_test_t */
    ngx_array_t                         tests;
} ngx_http_hc_match_t;


/* A header line of a check response. */
typedef struct {
    ngx_str_t                           name;
    ngx_str_t                           value;
} ngx_http_hc_header_t;


/* Health of one server address, kept by worker 0. */
typedef struct {
    struct sockaddr                    *sockaddr;
    socklen_t                           socklen;
    uint64_t                            failing;   /* one bit per check */
} ngx_http_hc_peer_t;


/* An upstream group checked by one or more health_check directives. */
typedef struct {
    ngx_http_upstream_srv_conf_t       *uscf;
    /*
     * server lines without "resolve" marked "down" in the configuration
     * (ngx_http_upstream_server_t *); servers added by "resolve" take their
     * state from their template, see ngx_http_hc_admin_down()
     */
    ngx_array_t                         down;
    /* ngx_http_hc_peer_t */
    ngx_array_t                        *peers;
    ngx_uint_t                          nchecks;
} ngx_http_hc_upstream_t;


/* One health_check directive. */
typedef struct {
    ngx_msec_t                          interval;
    ngx_str_t                           uri;
    ngx_str_t                           match_name;
    ngx_http_hc_match_t                 *match;

    void                              **loc_conf;
    ngx_str_t                           file;
    ngx_uint_t                          line;

    ngx_http_hc_upstream_t              *upstream;
    ngx_uint_t                          index;     /* bit in peer->failing */
    ngx_msec_t                          connect_timeout;
    ngx_msec_t                          send_timeout;
    ngx_msec_t                          read_timeout;
    /* HTTP/1.1 request (proxy_http_version 1.1 or 2), else HTTP/1.0 */
    ngx_flag_t                          http11;
    ngx_str_t                           request;

#if (NGX_HTTP_SSL)
    /* proxy_pass https://: the location's SSL context, NULL for http:// */
    ngx_ssl_t                          *ssl;
    /* name the certificate must match if ssl_verify, without a port */
    ngx_str_t                           ssl_name;
    /* sent as SNI, null-terminated; NULL if none */
    u_char                             *ssl_server_name;
    ngx_flag_t                          ssl_verify;
#endif

    ngx_event_t                         timer;
    ngx_uint_t                          pending;
} ngx_http_hc_t;


typedef struct {
    /* ngx_http_hc_match_t */
    ngx_array_t                         matches;
    /* ngx_http_hc_t * */
    ngx_array_t                         checks;
    /* ngx_http_hc_upstream_t * */
    ngx_array_t                         upstreams;
} ngx_http_hc_main_conf_t;


extern ngx_module_t  ngx_http_health_check_module;


char *ngx_http_hc_match_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
ngx_http_hc_match_t *ngx_http_hc_match_find(ngx_array_t *matches,
    ngx_str_t *name);
u_char *ngx_http_hc_match_response(ngx_http_hc_match_t *match,
    ngx_pool_t *pool, ngx_uint_t status, ngx_array_t *headers,
    ngx_str_t *body);

ngx_int_t ngx_http_hc_init_process(ngx_cycle_t *cycle);


#endif /* _NGX_HTTP_HEALTH_CHECK_H_INCLUDED_ */
