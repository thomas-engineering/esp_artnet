#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_http_server.h"

/* Reads the complete request body into buf and terminates it with '\0'.
   Returns the body length, or -1 if the body is empty, does not fit into
   buf (size includes the terminator) or the connection failed. */
int http_recv_body(httpd_req_t *req, char *buf, size_t size);

/* CSRF guard for state-changing POST handlers. Requires the custom header
   "X-Requested-With: dmx-node" (a cross-origin page cannot set it without a
   CORS preflight, which this server never grants) and, when the browser
   sends an Origin header, that its host matches the Host header.
   On failure sends 403 and returns false: the handler must return at once. */
bool http_check_csrf(httpd_req_t *req);
