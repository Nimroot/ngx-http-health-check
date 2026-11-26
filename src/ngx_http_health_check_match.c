/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The "match" directive: parsing its tests and running them against a
 * check response.
 */


#include "ngx_http_health_check.h"


static char *ngx_http_hc_match_test(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);
static ngx_regex_t *ngx_http_hc_regex(ngx_conf_t *cf, ngx_str_t *pattern);
static u_char *ngx_http_hc_run_test(ngx_http_hc_test_t *t, ngx_pool_t *pool,
    ngx_uint_t status, ngx_array_t *headers, ngx_str_t *body);


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

    if (ngx_http_hc_match_find(&hmcf->matches, &value[1])) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate match \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    match = ngx_array_push(&hmcf->matches);
    if (match == NULL) {
        return NGX_CONF_ERROR;
    }

    match->name = value[1];

    if (ngx_array_init(&match->tests, cf->pool, 4,
                       sizeof(ngx_http_hc_test_t))
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


/* One line inside a match block: status, header or body. */

static char *
ngx_http_hc_match_test(ngx_conf_t *cf, ngx_command_t *dummy, void *conf)
{
    ngx_http_hc_match_t *match = conf;

    u_char               *p, *dash;
    size_t                len;
    ngx_int_t             low, high;
    ngx_str_t            *value, *op;
    ngx_uint_t            i, n;
    ngx_http_hc_test_t   *t;
    ngx_http_hc_range_t  *range;

    value = cf->args->elts;
    n = cf->args->nelts;

    t = ngx_array_push(&match->tests);
    if (t == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(t, sizeof(ngx_http_hc_test_t));

    /* keep the test as written, for log messages */

    len = 0;
    for (i = 0; i < n; i++) {
        len += value[i].len + 1;
    }

    t->text.data = ngx_pnalloc(cf->pool, len);
    if (t->text.data == NULL) {
        return NGX_CONF_ERROR;
    }

    p = t->text.data;
    for (i = 0; i < n; i++) {
        if (i) {
            *p++ = ' ';
        }
        p = ngx_cpymem(p, value[i].data, value[i].len);
    }
    t->text.len = p - t->text.data;

    if (ngx_strcmp(value[0].data, "status") == 0) {
        t->type = NGX_HTTP_HC_STATUS;

        i = 1;
        if (n > 1 && ngx_strcmp(value[1].data, "!") == 0) {
            t->negate = 1;
            i = 2;
        }

        if (i == n) {
            goto invalid_args;
        }

        t->ranges = ngx_array_create(cf->pool, n - i,
                                     sizeof(ngx_http_hc_range_t));
        if (t->ranges == NULL) {
            return NGX_CONF_ERROR;
        }

        for ( /* void */ ; i < n; i++) {
            dash = ngx_strlchr(value[i].data, value[i].data + value[i].len,
                               '-');
            if (dash) {
                low = ngx_atoi(value[i].data, dash - value[i].data);
                high = ngx_atoi(dash + 1,
                                value[i].data + value[i].len - dash - 1);
            } else {
                low = ngx_atoi(value[i].data, value[i].len);
                high = low;
            }

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
        t->type = NGX_HTTP_HC_HEADER;

        if (n == 2 && ngx_strcmp(value[1].data, "!") != 0) {
            t->op = NGX_HTTP_HC_HEADER_PRESENT;
            t->name = value[1];
            return NGX_CONF_OK;
        }

        if (n == 3 && ngx_strcmp(value[1].data, "!") == 0) {
            t->op = NGX_HTTP_HC_HEADER_ABSENT;
            t->name = value[2];
            return NGX_CONF_OK;
        }

        if (n != 4) {
            goto invalid_args;
        }

        t->name = value[1];
        t->value = value[3];
        op = &value[2];

        if (ngx_strcmp(op->data, "=") == 0) {
            t->op = NGX_HTTP_HC_HEADER_EQ;

        } else if (ngx_strcmp(op->data, "!=") == 0) {
            t->op = NGX_HTTP_HC_HEADER_NE;

        } else if (ngx_strcmp(op->data, "~") == 0) {
            t->op = NGX_HTTP_HC_HEADER_RE;

        } else if (ngx_strcmp(op->data, "!~") == 0) {
            t->op = NGX_HTTP_HC_HEADER_NRE;

        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid operator \"%V\" in \"header\"", op);
            return NGX_CONF_ERROR;
        }

        if (t->op == NGX_HTTP_HC_HEADER_RE
            || t->op == NGX_HTTP_HC_HEADER_NRE)
        {
            t->regex = ngx_http_hc_regex(cf, &value[3]);
            if (t->regex == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        return NGX_CONF_OK;
    }

    if (ngx_strcmp(value[0].data, "body") == 0) {
        t->type = NGX_HTTP_HC_BODY;

        if (n != 3) {
            goto invalid_args;
        }

        if (ngx_strcmp(value[1].data, "!~") == 0) {
            t->negate = 1;

        } else if (ngx_strcmp(value[1].data, "~") != 0) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid operator \"%V\" in \"body\"",
                               &value[1]);
            return NGX_CONF_ERROR;
        }

        t->regex = ngx_http_hc_regex(cf, &value[2]);
        if (t->regex == NULL) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
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

invalid_args:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid number of arguments in \"%V\" test",
                       &value[0]);
    return NGX_CONF_ERROR;
}


static ngx_regex_t *
ngx_http_hc_regex(ngx_conf_t *cf, ngx_str_t *pattern)
{
    u_char               errstr[NGX_MAX_CONF_ERRSTR];
    ngx_regex_compile_t  rc;

    ngx_memzero(&rc, sizeof(ngx_regex_compile_t));

    rc.pattern = *pattern;
    rc.pool = cf->pool;
    rc.err.len = NGX_MAX_CONF_ERRSTR;
    rc.err.data = errstr;

    if (ngx_regex_compile(&rc) != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%V", &rc.err);
        return NULL;
    }

    return rc.regex;
}


/* The match block with this name in "matches", or NULL. */

ngx_http_hc_match_t *
ngx_http_hc_match_find(ngx_array_t *matches, ngx_str_t *name)
{
    ngx_uint_t            i;
    ngx_http_hc_match_t  *match;

    match = matches->elts;

    for (i = 0; i < matches->nelts; i++) {
        if (match[i].name.len == name->len
            && ngx_strncmp(match[i].name.data, name->data, name->len) == 0)
        {
            return &match[i];
        }
    }

    return NULL;
}


/*
 * NULL if the response passes every test of "match" (without a match block:
 * if its status is 2xx or 3xx), otherwise why it failed. "headers" holds
 * ngx_keyval_t; the reason is allocated from "pool".
 */

u_char *
ngx_http_hc_match_response(ngx_http_hc_match_t *match, ngx_pool_t *pool,
    ngx_uint_t status, ngx_array_t *headers, ngx_str_t *body)
{
    u_char              *reason;
    ngx_uint_t           i;
    ngx_http_hc_test_t  *t;

    if (match == NULL) {
        if (status >= 200 && status < 400) {
            return NULL;
        }

        reason = ngx_pnalloc(pool, 64);
        if (reason == NULL) {
            return (u_char *) "bad status";
        }

        ngx_sprintf(reason, "status %ui is not 2xx or 3xx%Z", status);

        return reason;
    }

    t = match->tests.elts;

    for (i = 0; i < match->tests.nelts; i++) {
        reason = ngx_http_hc_run_test(&t[i], pool, status, headers, body);
        if (reason) {
            return reason;
        }
    }

    return NULL;
}


/* NULL if the response passes the test, otherwise why it failed. */

static u_char *
ngx_http_hc_run_test(ngx_http_hc_test_t *t, ngx_pool_t *pool,
    ngx_uint_t status, ngx_array_t *headers, ngx_str_t *body)
{
    u_char               *reason;
    ngx_int_t             rc;
    ngx_uint_t            i, found, pass;
    ngx_keyval_t         *h;
    ngx_http_hc_range_t  *range;

    pass = 0;

    switch (t->type) {

    case NGX_HTTP_HC_STATUS:
        range = t->ranges->elts;
        found = 0;

        for (i = 0; i < t->ranges->nelts; i++) {
            if (status >= range[i].low && status <= range[i].high) {
                found = 1;
                break;
            }
        }

        pass = t->negate ? !found : found;
        break;

    case NGX_HTTP_HC_HEADER:
        h = headers->elts;
        found = 0;

        for (i = 0; i < headers->nelts; i++) {
            if (h[i].key.len != t->name.len
                || ngx_strncasecmp(h[i].key.data, t->name.data, t->name.len)
                   != 0)
            {
                continue;
            }

            found = 1;

            switch (t->op) {

            case NGX_HTTP_HC_HEADER_EQ:
            case NGX_HTTP_HC_HEADER_NE:
                rc = (h[i].value.len == t->value.len
                      && ngx_strncmp(h[i].value.data, t->value.data,
                                     t->value.len) == 0);
                if (rc == (t->op == NGX_HTTP_HC_HEADER_EQ)) {
                    pass = 1;
                }
                break;

            case NGX_HTTP_HC_HEADER_RE:
            case NGX_HTTP_HC_HEADER_NRE:
                rc = ngx_regex_exec(t->regex, &h[i].value, NULL, 0);
                if (rc < NGX_REGEX_NO_MATCHED) {
                    break;
                }
                if ((rc >= 0) == (t->op == NGX_HTTP_HC_HEADER_RE)) {
                    pass = 1;
                }
                break;
            }
        }

        if (t->op == NGX_HTTP_HC_HEADER_PRESENT) {
            pass = found;

        } else if (t->op == NGX_HTTP_HC_HEADER_ABSENT) {
            pass = !found;
        }

        break;

    case NGX_HTTP_HC_BODY:
        rc = ngx_regex_exec(t->regex, body, NULL, 0);
        if (rc < NGX_REGEX_NO_MATCHED) {
            return (u_char *) "regex error in body test";
        }

        pass = t->negate ? (rc == NGX_REGEX_NO_MATCHED) : (rc >= 0);
        break;
    }

    if (pass) {
        return NULL;
    }

    reason = ngx_pnalloc(pool, t->text.len + 64);
    if (reason == NULL) {
        return (u_char *) "test failed";
    }

    if (t->type == NGX_HTTP_HC_STATUS) {
        ngx_sprintf(reason, "status %ui fails \"%V\"%Z", status, &t->text);

    } else {
        ngx_sprintf(reason, "response fails \"%V\"%Z", &t->text);
    }

    return reason;
}
