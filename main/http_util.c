#include "http_util.h"

int http_recv_body(httpd_req_t *req, char *buf, size_t size)
{
    size_t total = req->content_len;
    if (total == 0 || total >= size) return -1;

    size_t got = 0;
    int timeouts = 0;
    while (got < total) {
        /* One call may return only part of the body */
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    buf[got] = '\0';
    return (int)got;
}
