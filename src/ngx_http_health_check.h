/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Types shared by the module's source files:
 *
 *     ngx_http_health_check_module.c     directives, configuration
 *     ngx_http_health_check_match.c      match blocks and their tests
 *     ngx_http_health_check_probe.c      probing servers, recording health
 *     ngx_http_health_check_response.c   reading and parsing a response
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


/*
 * A server's health is kept where every load-balancing method looks: its
 * "down" field, in the group's zone. Each health_check of the group owns
 * one bit there, above the values nginx uses itself ("down" on the server
 * line is 1, "drain" is 8). A server is used while "down" is 0.
 */

#define NGX_HTTP_HC_CONF_DOWN   1
#define NGX_HTTP_HC_SHIFT       16
#define NGX_HTTP_HC_MASK        ((ngx_uint_t) -1 << NGX_HTTP_HC_SHIFT)
#define NGX_HTTP_HC_MAX         (sizeof(ngx_uint_t) * 8 - NGX_HTTP_HC_SHIFT)

#define NGX_HTTP_HC_BODY_MAX    (256 * 1024)     /* body bytes examined */


typedef struct {
    ngx_str_t                       name;
    ngx_array_t                     tests;       /* ngx_http_hc_test_t */
    ngx_uint_t                      body;        /* has body tests */
} ngx_http_hc_match_t;


/* One health_check directive. */

typedef struct {
    ngx_http_upstream_srv_conf_t   *uscf;
    ngx_http_upstream_conf_t       *conf;        /* the location's proxy_* */
    ngx_http_hc_match_t            *match;
    ngx_uint_t                      bit;         /* in peer->down */
    ngx_msec_t                      interval;
    ngx_str_t                       uri;
    ngx_str_t                       request;

#if (NGX_HTTP_SSL)
    ngx_ssl_t                      *ssl;         /* NULL for http:// */
    ngx_str_t                       ssl_name;
    u_char                         *ssl_server_name;
#endif

    ngx_str_t                       match_name;
    void                          **loc_conf;
    ngx_str_t                       file;
    ngx_uint_t                      line;

    ngx_event_t                     timer;
    ngx_rbtree_t                    probes;      /* in progress, by peer */
    ngx_rbtree_node_t               sentinel;
} ngx_http_hc_t;


typedef struct {
    ngx_array_t                     matches;     /* ngx_http_hc_match_t */
    ngx_array_t                     checks;      /* ngx_http_hc_t */
} ngx_http_hc_main_conf_t;


/* The response to a probe, as it is read. */

typedef struct {
    ngx_pool_t                     *pool;
    ngx_http_request_t             *r;           /* nginx's parsers' state */
    ngx_buf_t                       buf;
    ngx_uint_t                      phase;
    ngx_http_status_t               status;
    ngx_array_t                     headers;     /* ngx_keyval_t */
    off_t                           content_length;
    ngx_http_chunked_t              chunk;
    off_t                           length;      /* body bytes so far */
    ngx_str_t                       body;        /* kept for body tests */
    size_t                          body_size;
    const char                     *error;       /* why it is rejected */
    unsigned                        chunked:1;
    unsigned                        keep_body:1;
} ngx_http_hc_response_t;


extern ngx_module_t  ngx_http_health_check_module;


char *ngx_http_hc_match_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
ngx_http_hc_match_t *ngx_http_hc_match_find(ngx_http_hc_main_conf_t *hmcf,
    ngx_str_t *name);
u_char *ngx_http_hc_match(ngx_http_hc_match_t *match,
    ngx_http_hc_response_t *resp, u_char *buf, size_t len);

ngx_int_t ngx_http_hc_response_init(ngx_http_hc_response_t *resp,
    ngx_connection_t *c, size_t size, ngx_uint_t keep_body);
ngx_int_t ngx_http_hc_response_recv(ngx_http_hc_response_t *resp,
    ngx_connection_t *c);

ngx_int_t ngx_http_hc_init_process(ngx_cycle_t *cycle);
void ngx_http_hc_exit_process(ngx_cycle_t *cycle);


#endif /* _NGX_HTTP_HEALTH_CHECK_H_INCLUDED_ */
