#pragma once
#include <stddef.h>
#include "esp_http_server.h"

/* Reads the complete request body into buf and terminates it with '\0'.
   Returns the body length, or -1 if the body is empty, does not fit into
   buf (size includes the terminator) or the connection failed. */
int http_recv_body(httpd_req_t *req, char *buf, size_t size);
