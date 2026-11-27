/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Reading and parsing the response to a probe with nginx's own parsers, as
 * the proxy does: the status line (interim 1xx responses are skipped), the
 * header lines, and the body as framed by Content-Length, by chunked
 * Transfer-Encoding, or by the end of the connection.
 *
 * Each header must fit in the buffer (proxy_buffer_size, as for requests):
 * after an interim response, what follows it is moved to the start of the
 * buffer, as the proxy does. The buffer is then reused for the body, which
 * only passes through: only its first NGX_HTTP_HC_BODY_MAX bytes are read,
 * and kept if a body test needs them.
 */


#include "ngx_http_health_check.h"


enum {
    NGX_HTTP_HC_STATUS_LINE = 0,
    NGX_HTTP_HC_HEADER_LINES,
    NGX_HTTP_HC_BODY_BYTES
};


static void ngx_http_hc_response_reset(ngx_http_hc_response_t *resp);
static ngx_int_t ngx_http_hc_response_parse(ngx_http_hc_response_t *resp);
static ngx_int_t ngx_http_hc_response_header(ngx_http_hc_response_t *resp);
static ngx_int_t ngx_http_hc_response_body(ngx_http_hc_response_t *resp);


ngx_int_t
ngx_http_hc_response_init(ngx_http_hc_response_t *resp, ngx_connection_t *c,
    size_t size, ngx_uint_t keep_body)
{
    resp->pool = c->pool;
    resp->r = ngx_pcalloc(c->pool, sizeof(ngx_http_request_t));
    resp->buf.start = ngx_palloc(c->pool, size);

    if (resp->r == NULL
        || resp->buf.start == NULL
        || ngx_array_init(&resp->headers, c->pool, 8, sizeof(ngx_keyval_t))
           != NGX_OK)
    {
        return NGX_ERROR;
    }

    resp->r->connection = c;               /* for the parsers' debug log */

    resp->buf.pos = resp->buf.start;
    resp->buf.last = resp->buf.start;
    resp->buf.end = resp->buf.start + size;

    resp->keep_body = keep_body;
    ngx_str_set(&resp->body, "");

    ngx_http_hc_response_reset(resp);

    return NGX_OK;
}


/*
 * Reads and parses what has arrived: NGX_AGAIN until the response is
 * complete, then NGX_OK; NGX_DECLINED if it is invalid or cut short, with
 * resp->error; NGX_ERROR if out of memory.
 */

ngx_int_t
ngx_http_hc_response_recv(ngx_http_hc_response_t *resp, ngx_connection_t *c)
{
    ssize_t     n;
    ngx_int_t   rc;
    ngx_buf_t  *b;

    b = &resp->buf;

    for ( ;; ) {

        if (resp->phase == NGX_HTTP_HC_BODY_BYTES) {
            b->pos = b->start;
            b->last = b->start;

        } else if (b->last == b->end) {
            resp->error = "response header too large";
            return NGX_DECLINED;
        }

        n = c->recv(c, b->last, b->end - b->last);

        if (n == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (n == NGX_ERROR) {
            resp->error = "recv() failed";
            return NGX_DECLINED;
        }

        if (n == 0) {
            if (resp->phase != NGX_HTTP_HC_BODY_BYTES) {
                resp->error = (b->last == b->start && resp->interim == 0)
                              ? "connection closed without a response"
                              : "connection closed before the response "
                                "header was complete";
                return NGX_DECLINED;
            }

            if (resp->chunked || resp->content_length != -1) {
                resp->error = "connection closed before the response body "
                              "was complete";
                return NGX_DECLINED;
            }

            return NGX_OK;                 /* the body ends with it */
        }

        b->last += n;

        rc = ngx_http_hc_response_parse(resp);

        if (rc != NGX_AGAIN) {
            return rc;
        }
    }
}


/* Ready for a response: the first one, or the one after an interim one. */

static void
ngx_http_hc_response_reset(ngx_http_hc_response_t *resp)
{
    resp->phase = NGX_HTTP_HC_STATUS_LINE;
    ngx_memzero(&resp->status, sizeof(ngx_http_status_t));
    resp->headers.nelts = 0;
    resp->content_length = -1;
    resp->chunked = 0;
}


static ngx_int_t
ngx_http_hc_response_parse(ngx_http_hc_response_t *resp)
{
    ngx_int_t    rc;
    ngx_buf_t   *b;
    ngx_uint_t   code;

    b = &resp->buf;

    for ( ;; ) {

        switch (resp->phase) {

        case NGX_HTTP_HC_STATUS_LINE:

            rc = ngx_http_parse_status_line(resp->r, b, &resp->status);

            if (rc == NGX_AGAIN) {
                return NGX_AGAIN;
            }

            if (rc != NGX_OK || resp->r->http_major != 1) {

                /* a TLS record: an https server asked with http:// */

                resp->error = (*b->start >= 0x14 && *b->start <= 0x17)
                              ? "TLS data instead of an HTTP response "
                                "(an https server?)"
                              : "invalid status line";
                return NGX_DECLINED;
            }

            resp->phase = NGX_HTTP_HC_HEADER_LINES;
            break;

        case NGX_HTTP_HC_HEADER_LINES:

            rc = ngx_http_hc_response_header(resp);

            if (rc != NGX_OK) {
                return rc;
            }

            code = resp->status.code;

            /*
             * An interim response (RFC 9110, 15.2): the final one follows,
             * with the whole buffer for its header. Their number is
             * limited, as the read timeout starts again with each read.
             */

            if (code < 200 && code != 101) {

                if (++resp->interim > NGX_HTTP_HC_INTERIM_MAX) {
                    resp->error = "too many interim responses";
                    return NGX_DECLINED;
                }

                b->last = ngx_movemem(b->start, b->pos, b->last - b->pos);
                b->pos = b->start;

                ngx_http_hc_response_reset(resp);
                break;
            }

            if (code < 200 || code == 204 || code == 304
                || resp->content_length == 0)
            {
                return NGX_OK;
            }

            if (resp->keep_body) {
                resp->body_size = (resp->content_length == -1)
                                  ? NGX_HTTP_HC_BODY_MAX
                                  : (size_t) ngx_min(resp->content_length,
                                                     NGX_HTTP_HC_BODY_MAX);

                resp->body.data = ngx_pnalloc(resp->pool, resp->body_size);
                if (resp->body.data == NULL) {
                    return NGX_ERROR;
                }
            }

            resp->phase = NGX_HTTP_HC_BODY_BYTES;
            break;

        default: /* NGX_HTTP_HC_BODY_BYTES */
            return ngx_http_hc_response_body(resp);
        }
    }
}


/*
 * Header lines up to the empty one, copied, as the buffer is reused for
 * the body. As nginx's proxy, rejects a second Content-Length or
 * Transfer-Encoding, both together, and a transfer coding but "chunked".
 */

static ngx_int_t
ngx_http_hc_response_header(ngx_http_hc_response_t *resp)
{
    ngx_int_t            rc;
    ngx_uint_t           cl;
    ngx_keyval_t        *h;
    ngx_http_request_t  *r;

    r = resp->r;

    for ( ;; ) {

        rc = ngx_http_parse_header_line(r, &resp->buf, 1);

        if (rc == NGX_HTTP_PARSE_HEADER_DONE) {
            return NGX_OK;
        }

        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (rc != NGX_OK) {
            resp->error = "invalid response header";
            return NGX_DECLINED;
        }

        h = ngx_array_push(&resp->headers);
        if (h == NULL) {
            return NGX_ERROR;
        }

        h->key.len = r->header_name_end - r->header_name_start;
        h->value.len = r->header_end - r->header_start;

        h->key.data = ngx_pnalloc(resp->pool, h->key.len + h->value.len);
        if (h->key.data == NULL) {
            return NGX_ERROR;
        }

        h->value.data = ngx_cpymem(h->key.data, r->header_name_start,
                                   h->key.len);
        ngx_memcpy(h->value.data, r->header_start, h->value.len);

        cl = (h->key.len == sizeof("Content-Length") - 1
              && ngx_strncasecmp(h->key.data, (u_char *) "Content-Length",
                                 h->key.len) == 0);

        if (!cl
            && (h->key.len != sizeof("Transfer-Encoding") - 1
                || ngx_strncasecmp(h->key.data, (u_char *) "Transfer-Encoding",
                                   h->key.len) != 0))
        {
            continue;
        }

        if (resp->content_length != -1 || resp->chunked) {
            resp->error = (cl && resp->content_length != -1)
                          ? "duplicate Content-Length"
                          : (!cl && resp->chunked)
                          ? "duplicate Transfer-Encoding"
                          : "both Content-Length and Transfer-Encoding";
            return NGX_DECLINED;
        }

        if (cl) {
            resp->content_length = ngx_atoof(h->value.data, h->value.len);

            if (resp->content_length == NGX_ERROR) {
                resp->error = "invalid Content-Length";
                return NGX_DECLINED;
            }

            continue;
        }

        if (h->value.len != sizeof("chunked") - 1
            || ngx_strncasecmp(h->value.data, (u_char *) "chunked",
                               h->value.len) != 0)
        {
            resp->error = "unsupported Transfer-Encoding";
            return NGX_DECLINED;
        }

        resp->chunked = 1;
    }
}


/*
 * Takes in the body bytes in the buffer, decoding chunks: NGX_OK once the
 * body is complete or its first NGX_HTTP_HC_BODY_MAX bytes are in.
 */

static ngx_int_t
ngx_http_hc_response_body(ngx_http_hc_response_t *resp)
{
    off_t       n;
    size_t      keep;
    ngx_int_t   rc;
    ngx_buf_t  *b;

    b = &resp->buf;

    while (b->pos < b->last) {

        n = b->last - b->pos;

        if (resp->chunked) {
            rc = ngx_http_parse_chunked(resp->r, b, &resp->chunk, 0);

            if (rc == NGX_DONE) {
                return NGX_OK;
            }

            if (rc == NGX_ERROR) {
                resp->error = "invalid chunked body";
                return NGX_DECLINED;
            }

            if (rc == NGX_AGAIN) {
                return NGX_AGAIN;
            }

            /* NGX_OK: chunk data at b->pos */

            n = ngx_min(b->last - b->pos, resp->chunk.size);
            resp->chunk.size -= n;

        } else if (resp->content_length != -1) {
            n = ngx_min(n, resp->content_length - resp->length);
        }

        if (resp->body.len < resp->body_size) {
            keep = ngx_min((size_t) n, resp->body_size - resp->body.len);
            ngx_memcpy(resp->body.data + resp->body.len, b->pos, keep);
            resp->body.len += keep;
        }

        b->pos += n;
        resp->length += n;

        if (resp->length == resp->content_length
            || resp->length >= NGX_HTTP_HC_BODY_MAX)
        {
            return NGX_OK;
        }
    }

    return NGX_AGAIN;
}
