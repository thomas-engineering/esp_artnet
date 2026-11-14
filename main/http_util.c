#include "http_util.h"
#include <string.h>
#include "esp_log.h"

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

/* Copies header `name` into buf; false if missing or too long */
static bool get_hdr(httpd_req_t *req, const char *name, char *buf, size_t n)
{
    return httpd_req_get_hdr_value_str(req, name, buf, n) == ESP_OK;
}

bool http_check_csrf(httpd_req_t *req)
{
    char v[80];
    bool ok = get_hdr(req, "X-Requested-With", v, sizeof(v)) && strcmp(v, "dmx-node") == 0;

    char origin[96], host[64];
    if (ok && get_hdr(req, "Origin", origin, sizeof(origin))) {
        /* Origin = scheme://host[:port]; compare everything after "://" */
        const char *oh = strstr(origin, "://");
        ok = oh && get_hdr(req, "Host", host, sizeof(host)) &&
             strcasecmp(oh + 3, host) == 0;
    }
    if (!ok) {
        ESP_LOGW("http", "Rejected cross-site or malformed request to %s", req->uri);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Forbidden\"}");
    }
    return ok;
}
