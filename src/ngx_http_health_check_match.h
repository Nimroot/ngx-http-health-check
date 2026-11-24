/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The "match" directive: parsing its tests and running them against a
 * check response.
 */


#ifndef _NGX_HTTP_HEALTH_CHECK_MATCH_H_INCLUDED_
#define _NGX_HTTP_HEALTH_CHECK_MATCH_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


enum {
    NGX_HTTP_HEALTH_CHECK_STATUS = 0,
    NGX_HTTP_HEALTH_CHECK_HEADER,
    NGX_HTTP_HEALTH_CHECK_BODY
};


enum {
    NGX_HTTP_HEALTH_CHECK_HEADER_PRESENT = 0,   /* header Name;          */
    NGX_HTTP_HEALTH_CHECK_HEADER_ABSENT,        /* header ! Name;        */
    NGX_HTTP_HEALTH_CHECK_HEADER_EQ,            /* header Name = value;  */
    NGX_HTTP_HEALTH_CHECK_HEADER_NE,            /* header Name != value; */
    NGX_HTTP_HEALTH_CHECK_HEADER_RE,            /* header Name ~ regex;  */
    NGX_HTTP_HEALTH_CHECK_HEADER_NRE            /* header Name !~ regex; */
};


typedef struct {
    ngx_uint_t                          low;
    ngx_uint_t                          high;
} ngx_http_health_check_range_t;


typedef struct {
    ngx_uint_t                          type;
    ngx_uint_t                          op;        /* header tests */
    ngx_uint_t                          negate;    /* status and body tests */
    /* ngx_http_health_check_range_t */
    ngx_array_t                        *ranges;
    ngx_str_t                           name;
    ngx_str_t                           value;
    ngx_regex_t                        *regex;
    ngx_str_t                           text;      /* the test as written */
} ngx_http_health_check_test_t;


typedef struct {
    ngx_str_t                           name;
    /* ngx_http_health_check_test_t */
    ngx_array_t                         tests;
} ngx_http_health_check_match_t;


/* A header line of a check response. */
typedef struct {
    ngx_str_t                           name;
    ngx_str_t                           value;
} ngx_http_health_check_header_t;


char *ngx_http_health_check_match_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
ngx_http_health_check_match_t *ngx_http_health_check_match_find(
    ngx_array_t *matches, ngx_str_t *name);
u_char *ngx_http_health_check_match_response(
    ngx_http_health_check_match_t *match, ngx_pool_t *pool, ngx_uint_t status,
    ngx_array_t *headers, ngx_str_t *body);


#endif /* _NGX_HTTP_HEALTH_CHECK_MATCH_H_INCLUDED_ */
