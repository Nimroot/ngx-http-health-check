/*
 * Copyright (C) 2026 The ngx_http_health_check authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Probing the servers of a group and recording their health.
 */


#ifndef _NGX_HTTP_HEALTH_CHECK_PROBE_H_INCLUDED_
#define _NGX_HTTP_HEALTH_CHECK_PROBE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


/* Sets up probing in worker 0, before the first check runs. */
void ngx_http_health_check_probe_init(ngx_cycle_t *cycle);

/* Timer handler of a health check; ev->data is its ngx_http_health_check_t. */
void ngx_http_health_check_run(ngx_event_t *ev);


#endif /* _NGX_HTTP_HEALTH_CHECK_PROBE_H_INCLUDED_ */
