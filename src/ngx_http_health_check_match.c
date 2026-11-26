/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The match block: parsing its tests, and running them against a response.
 */


#include "ngx_http_health_check.h"


typedef enum {
    NGX_HTTP_HC_STATUS = 0,                      /* status [!] code|low-high */
    NGX_HTTP_HC_HAS_HEADER,                      /* header [!] name */
    NGX_HTTP_HC_HEADER,                          /* header name op value */
    NGX_HTTP_HC_BODY                             /* body [!]~ regex */
} ngx_http_hc_test_type_e;


typedef struct {
    ngx_uint_t                      low;
    ngx_uint_t                      high;
} ngx_http_hc_range_t;


typedef struct {
    ngx_http_hc_test_type_e         type;
    ngx_uint_t                      negate;
    ngx_array_t                    *ranges;      /* ngx_http_hc_range_t */
    ngx_str_t                       name;
    ngx_str_t                       value;       /* header =, != */
    ngx_regex_t                    *regex;       /* header ~, !~; body */
    ngx_str_t                       text;        /* as written, for the log */
} ngx_http_hc_test_t;


static char *ngx_http_hc_match_test(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);
static char *ngx_http_hc_test_op(ngx_conf_t *cf, ngx_http_hc_test_t *t,
    ngx_str_t *op, ngx_str_t *arg);
static ngx_uint_t ngx_http_hc_passes(ngx_http_hc_test_t *t, ngx_str_t *s);


/* match name { ... } */

char *
ngx_http_hc_match_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_hc_main_conf_t *hmcf = conf;

    char                 *rv;
    ngx_str_t            *value;
    ngx_conf_t            save;
    ngx_http_hc_match_t  *match;

    value = cf->args->elts;

    if (ngx_http_hc_match_find(hmcf, &value[1])) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate match \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    match = ngx_array_push(&hmcf->matches);
    if (match == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(match, sizeof(ngx_http_hc_match_t));

    match->name = value[1];

    if (ngx_array_init(&match->tests, cf->pool, 4, sizeof(ngx_http_hc_test_t))
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    save = *cf;
    cf->handler = ngx_http_hc_match_test;
    cf->handler_conf = (void *) match;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    return rv;
}


/* A line of a match block: a status, header or body test. */

static char *
ngx_http_hc_match_test(ngx_conf_t *cf, ngx_command_t *dummy, void *conf)
{
    ngx_http_hc_match_t *match = conf;

    u_char               *p, *dash, *last;
    ngx_int_t             low, high;
    ngx_str_t            *value;
    ngx_uint_t            i, n, neg;
    ngx_http_hc_test_t   *t;
    ngx_http_hc_range_t  *range;

    value = cf->args->elts;
    n = cf->args->nelts;

    t = ngx_array_push(&match->tests);
    if (t == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(t, sizeof(ngx_http_hc_test_t));

    t->text.len = n - 1;

    for (i = 0; i < n; i++) {
        t->text.len += value[i].len;
    }

    p = ngx_pnalloc(cf->pool, t->text.len);
    if (p == NULL) {
        return NGX_CONF_ERROR;
    }

    t->text.data = p;

    for (i = 0; i < n; i++) {
        p = ngx_cpymem(p, value[i].data, value[i].len);

        if (i < n - 1) {
            *p++ = ' ';
        }
    }

    /* "status ! ..." and "header ! name" */
    neg = (n > 2 && ngx_strcmp(value[1].data, "!") == 0);

    if (ngx_strcmp(value[0].data, "status") == 0) {
        if (n < 2 + neg) {
            goto invalid;
        }

        t->type = NGX_HTTP_HC_STATUS;
        t->negate = neg;

        t->ranges = ngx_array_create(cf->pool, n - 1 - neg,
                                     sizeof(ngx_http_hc_range_t));
        if (t->ranges == NULL) {
            return NGX_CONF_ERROR;
        }

        for (i = 1 + neg; i < n; i++) {
            last = value[i].data + value[i].len;
            dash = ngx_strlchr(value[i].data, last, '-');

            low = ngx_atoi(value[i].data, (dash ? dash : last) - value[i].data);
            high = dash ? ngx_atoi(dash + 1, last - dash - 1) : low;

            if (low < 100 || high > 599 || low > high) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid status \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            range = ngx_array_push(t->ranges);
            if (range == NULL) {
                return NGX_CONF_ERROR;
            }

            range->low = low;
            range->high = high;
        }

        return NGX_CONF_OK;
    }

    if (ngx_strcmp(value[0].data, "header") == 0) {

        if (n == 2 + neg) {
            t->type = NGX_HTTP_HC_HAS_HEADER;
            t->negate = neg;
            t->name = value[n - 1];
            return NGX_CONF_OK;
        }

        if (n != 4) {
            goto invalid;
        }

        t->type = NGX_HTTP_HC_HEADER;
        t->name = value[1];

        return ngx_http_hc_test_op(cf, t, &value[2], &value[3]);
    }

    if (ngx_strcmp(value[0].data, "body") == 0) {
        if (n != 3) {
            goto invalid;
        }

        t->type = NGX_HTTP_HC_BODY;

        return ngx_http_hc_test_op(cf, t, &value[1], &value[2]);
    }

    if (ngx_strcmp(value[0].data, "require") == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"require\" in \"match\" is not supported by "
                           "ngx_http_health_check_module");
        return NGX_CONF_ERROR;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "unknown test \"%V\" in \"match\"", &value[0]);
    return NGX_CONF_ERROR;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid number of arguments in \"%V\" test",
                       &value[0]);
    return NGX_CONF_ERROR;
}


/* The operator of a header or body test: =, !=, ~ or !~ (no = for body). */

static char *
ngx_http_hc_test_op(ngx_conf_t *cf, ngx_http_hc_test_t *t, ngx_str_t *op,
    ngx_str_t *arg)
{
    u_char               errstr[NGX_MAX_CONF_ERRSTR];
    ngx_regex_compile_t  rc;

    t->negate = (op->len == 2 && op->data[0] == '!');

    if (op->len == 1 + t->negate
        && op->data[t->negate] == '='
        && t->type == NGX_HTTP_HC_HEADER)
    {
        t->value = *arg;
        return NGX_CONF_OK;
    }

    if (op->len != 1 + t->negate || op->data[t->negate] != '~') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid operator \"%V\"", op);
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&rc, sizeof(ngx_regex_compile_t));

    rc.pattern = *arg;
    rc.pool = cf->pool;
    rc.err.len = NGX_MAX_CONF_ERRSTR;
    rc.err.data = errstr;

    if (ngx_regex_compile(&rc) != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%V", &rc.err);
        return NGX_CONF_ERROR;
    }

    t->regex = rc.regex;

    return NGX_CONF_OK;
}


ngx_http_hc_match_t *
ngx_http_hc_match_find(ngx_http_hc_main_conf_t *hmcf, ngx_str_t *name)
{
    ngx_uint_t            i;
    ngx_http_hc_match_t  *match;

    match = hmcf->matches.elts;

    for (i = 0; i < hmcf->matches.nelts; i++) {
        if (match[i].name.len == name->len
            && ngx_strncmp(match[i].name.data, name->data, name->len) == 0)
        {
            return &match[i];
        }
    }

    return NULL;
}


/*
 * NULL if a complete response passes every test of "match" (without a
 * match block: if its status is 2xx or 3xx); otherwise why not, in buf.
 */

u_char *
ngx_http_hc_match(ngx_http_hc_match_t *match, ngx_uint_t code,
    ngx_array_t *headers, ngx_str_t *body, u_char *buf, size_t len)
{
    u_char               *last;
    ngx_uint_t            i, j, pass;
    ngx_keyval_t         *h;
    ngx_http_hc_test_t   *t;
    ngx_http_hc_range_t  *range;

    if (match == NULL) {
        if (code >= 200 && code < 400) {
            return NULL;
        }

        last = ngx_snprintf(buf, len - 1, "status %ui is not 2xx or 3xx",
                            code);
        *last = '\0';

        return buf;
    }

    t = match->tests.elts;
    h = headers->elts;

    for (i = 0; i < match->tests.nelts; i++) {

        pass = 0;

        switch (t[i].type) {

        case NGX_HTTP_HC_STATUS:
            range = t[i].ranges->elts;

            for (j = 0; j < t[i].ranges->nelts && !pass; j++) {
                pass = (code >= range[j].low && code <= range[j].high);
            }

            pass ^= t[i].negate;
            break;

        case NGX_HTTP_HC_HAS_HEADER:
        case NGX_HTTP_HC_HEADER:

            /* "header name != value": one with another value */

            for (j = 0; j < headers->nelts && !pass; j++) {
                pass = (h[j].key.len == t[i].name.len
                        && ngx_strncasecmp(h[j].key.data, t[i].name.data,
                                           t[i].name.len) == 0
                        && (t[i].type == NGX_HTTP_HC_HAS_HEADER
                            || ngx_http_hc_passes(&t[i], &h[j].value)));
            }

            if (t[i].type == NGX_HTTP_HC_HAS_HEADER) {
                pass ^= t[i].negate;
            }

            break;

        case NGX_HTTP_HC_BODY:
            pass = ngx_http_hc_passes(&t[i], body);
            break;
        }

        if (pass) {
            continue;
        }

        if (t[i].type == NGX_HTTP_HC_STATUS) {
            last = ngx_snprintf(buf, len - 1, "status %ui fails \"%V\"",
                                code, &t[i].text);

        } else {
            last = ngx_snprintf(buf, len - 1, "response fails \"%V\"",
                                &t[i].text);
        }

        *last = '\0';

        return buf;
    }

    return NULL;
}


/* A header value or the body against =, !=, ~ or !~; an error fails. */

static ngx_uint_t
ngx_http_hc_passes(ngx_http_hc_test_t *t, ngx_str_t *s)
{
    ngx_int_t  rc;

    if (t->regex == NULL) {
        return (s->len == t->value.len
                && ngx_memcmp(s->data, t->value.data, s->len) == 0)
               ^ t->negate;
    }

    rc = ngx_regex_exec(t->regex, s, NULL, 0);

    return rc >= NGX_REGEX_NO_MATCHED && ((rc >= 0) ^ t->negate);
}
