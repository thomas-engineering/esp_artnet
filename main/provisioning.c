#include "provisioning.h"
#include "led.h"
#include "http_util.h"
#include <string.h>
#include <stdlib.h>
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define NVS_NAMESPACE    "prov"
#define NVS_KEY_SSID     "ssid"
#define NVS_KEY_PASS     "pass"
#define NVS_KEY_UNIVERSE "universe"
#define NVS_KEY_IP_MODE  "ip_mode"
#define NVS_KEY_S_IP     "s_ip"
#define NVS_KEY_S_NM     "s_nm"
#define NVS_KEY_S_GW     "s_gw"
#define NVS_KEY_S_DNS    "s_dns"
#define AP_SSID_PREFIX  "ARTNET_"
#define PORTAL_IP       "192.168.4.1"
#define WIFI_CONN_BIT   BIT0
#define WIFI_FAIL_BIT   BIT1
#define MAX_RETRIES     5

/* IEEE 802.11 limits; buffers hold the value plus a terminator */
#define SSID_MAX_LEN    32
#define PASS_MAX_LEN    64

/* Reconnect back-off after the connection was lost at runtime */
#define RECONNECT_MIN_MS    1000
#define RECONNECT_MAX_MS    30000
/* Portal fallback with stored credentials: retry the connection this often */
#define FALLBACK_RETRY_MS   30000

static const char *TAG = "prov";
static EventGroupHandle_t s_eg;
static int  s_retry = 0;
static bool s_sta_connecting = false;
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_sta_netif = NULL;

/* Set once the node is online: from then on a lost connection is retried forever */
static bool s_online = false;
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;

/* Network configuration is cached in RAM (read on every status request) */
static net_config_t s_netcfg;
static bool s_netcfg_loaded = false;

/* ── NVS helpers ──────────────────────────────────────────────────────── */

static bool load_credentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_sz) == ESP_OK &&
               nvs_get_str(h, NVS_KEY_PASS, pass, &pass_sz) == ESP_OK &&
               ssid[0] != '\0');
    nvs_close(h);
    return ok;
}

static void save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

void provisioning_save_universe(uint16_t univ)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, NVS_KEY_UNIVERSE, univ);
    nvs_commit(h);
    nvs_close(h);
}

void provisioning_get_net_config(net_config_t *cfg)
{
    if (s_netcfg_loaded) {
        *cfg = s_netcfg;
        return;
    }
    cfg->dhcp = true;
    cfg->ip[0] = cfg->netmask[0] = cfg->gateway[0] = cfg->dns[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t mode = 0;
    nvs_get_u8(h, NVS_KEY_IP_MODE, &mode);
    cfg->dhcp = (mode == 0);

    size_t sz;
    sz = sizeof(cfg->ip);      nvs_get_str(h, NVS_KEY_S_IP,  cfg->ip,      &sz);
    sz = sizeof(cfg->netmask); nvs_get_str(h, NVS_KEY_S_NM,  cfg->netmask, &sz);
    sz = sizeof(cfg->gateway); nvs_get_str(h, NVS_KEY_S_GW,  cfg->gateway, &sz);
    sz = sizeof(cfg->dns);     nvs_get_str(h, NVS_KEY_S_DNS, cfg->dns,     &sz);

    nvs_close(h);
    s_netcfg = *cfg;
    s_netcfg_loaded = true;
}

void provisioning_save_net_config(const net_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_IP_MODE, cfg->dhcp ? 0 : 1);
    if (!cfg->dhcp) {
        nvs_set_str(h, NVS_KEY_S_IP,  cfg->ip);
        nvs_set_str(h, NVS_KEY_S_NM,  cfg->netmask);
        nvs_set_str(h, NVS_KEY_S_GW,  cfg->gateway);
        nvs_set_str(h, NVS_KEY_S_DNS, cfg->dns);
    }
    nvs_commit(h);
    nvs_close(h);
    s_netcfg_loaded = false;        /* re-read: DHCP mode keeps the old static values */
}

uint16_t provisioning_get_universe(void)
{
    nvs_handle_t h;
    uint16_t univ = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u16(h, NVS_KEY_UNIVERSE, &univ);
    nvs_close(h);
    return univ;
}

void provisioning_clear(void)
{
    /* Factory reset: WiFi + network + universe, and the settings the other
       modules keep (Art-Net names / failsafe, automatic-update flag). */
    static const char *const namespaces[] = { NVS_NAMESPACE, "artnet", "ota" };
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); i++) {
        nvs_handle_t h;
        if (nvs_open(namespaces[i], NVS_READWRITE, &h) != ESP_OK) continue;
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_netcfg_loaded = false;
}

/* ── WiFi event handler ───────────────────────────────────────────────── */

static void apply_static_ip(const net_config_t *cfg)
{
    if (!s_sta_netif) return;
    esp_netif_dhcpc_stop(s_sta_netif);

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = inet_addr(cfg->ip);
    ip.netmask.addr = inet_addr(cfg->netmask);
    ip.gw.addr      = inet_addr(cfg->gateway);
    if (esp_netif_set_ip_info(s_sta_netif, &ip) != ESP_OK)
        ESP_LOGE(TAG, "Failed to set static IP");

    if (cfg->dns[0]) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = inet_addr(cfg->dns);
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    ESP_LOGI(TAG, "Static IP: %s / %s  gw %s", cfg->ip, cfg->netmask, cfg->gateway);
}

static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_connecting) {
            net_config_t netcfg;
            provisioning_get_net_config(&netcfg);
            if (!netcfg.dhcp) apply_static_ip(&netcfg);
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_connecting) {
            /* First connection attempt at boot: limited retries, then portal */
            if (s_retry < MAX_RETRIES) {
                esp_wifi_connect();
                s_retry++;
                ESP_LOGW(TAG, "Reconnecting (%d/%d)...", s_retry, MAX_RETRIES);
            } else {
                xEventGroupSetBits(s_eg, WIFI_FAIL_BIT);
            }
        } else if (s_online) {
            /* Connection lost at runtime (router restart, weak signal):
               keep trying forever, with a growing pause between attempts. */
            led_set_state(LED_CONNECTING);
            ESP_LOGW(TAG, "WiFi connection lost — reconnecting in %u ms",
                     (unsigned)s_backoff_ms);
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
            s_backoff_ms *= 2;
            if (s_backoff_ms > RECONNECT_MAX_MS) s_backoff_ms = RECONNECT_MAX_MS;
        }
        /* Portal fallback: provisioning_start() retries on its own schedule */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Connected — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_backoff_ms = RECONNECT_MIN_MS;
        if (s_online) led_set_state(LED_CONNECTED);
        xEventGroupSetBits(s_eg, WIFI_CONN_BIT);
    }
}

/* ── DNS task: responds to every query with PORTAL_IP ────────────────── */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "DNS hijack listening on port 53");

    static const uint8_t PORTAL_A[4] = {192, 168, 4, 1};
    uint8_t buf[512], resp[600];
    struct sockaddr_in client;
    socklen_t clen;

    while (1) {
        clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue;

        /* Find the end of the first question: labels, zero byte, QTYPE, QCLASS */
        int q = 12;
        while (q < n && buf[q] != 0) q += buf[q] + 1;
        int qend = q + 5;
        if (buf[q] != 0 || qend > n) continue;          /* malformed */
        uint16_t qtype = ((uint16_t)buf[q + 1] << 8) | buf[q + 2];

        /* Response = header + that one question (+ answer); anything the
           client appended after it (e.g. EDNS records) is dropped. */
        memcpy(resp, buf, qend);
        resp[2] = 0x81;        /* QR=1, AA=1 */
        resp[3] = 0x80;        /* RA=1, RCODE=0 */
        resp[4] = 0x00; resp[5] = 0x01;                 /* QDCOUNT = 1 */
        resp[6] = 0x00; resp[7] = 0x00;                 /* ANCOUNT = 0 */
        resp[8] = resp[9] = resp[10] = resp[11] = 0;    /* NSCOUNT, ARCOUNT */
        int rlen = qend;

        if (qtype == 1) {
            /* A query: answer with the portal address (name = ptr to offset 12).
               Other types (AAAA, ...) get an empty answer, so clients fall back to A. */
            resp[7] = 0x01;                            /* ANCOUNT = 1 */
            resp[rlen++] = 0xc0; resp[rlen++] = 0x0c;  /* NAME ptr */
            resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* TYPE  = A */
            resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* CLASS = IN */
            resp[rlen++] = 0x00; resp[rlen++] = 0x00;
            resp[rlen++] = 0x00; resp[rlen++] = 0x3c;  /* TTL   = 60 s */
            resp[rlen++] = 0x00; resp[rlen++] = 0x04;  /* RDLEN = 4 */
            memcpy(resp + rlen, PORTAL_A, 4); rlen += 4;
        }

        sendto(sock, resp, rlen, 0, (struct sockaddr *)&client, clen);
    }
}

/* ── Portal HTML ──────────────────────────────────────────────────────── */

/* Portal page: web/portal.html, embedded at build time (EMBED_TXTFILES).
   It contains the placeholders {{NET}}, {{SUB}}, {{UNI}} and {{VERSION}}; all visible
   strings are set by the bilingual JS block inside the page. */
extern const char portal_html_start[] asm("_binary_portal_html_start");

/* ── HTTP handlers ────────────────────────────────────────────────────── */

/* Sends the portal page in chunks, replacing the {{...}} placeholders */
static void send_portal_page(httpd_req_t *req, const char *net, const char *sub,
                             const char *uni, const char *version)
{
    static const struct { const char *key; size_t len; } keys[] = {
        { "{{NET}}", 7 }, { "{{SUB}}", 7 }, { "{{UNI}}", 7 }, { "{{VERSION}}", 11 },
    };
    const char *vals[] = { net, sub, uni, version };

    const char *p = portal_html_start;
    for (;;) {
        const char *k = strstr(p, "{{");
        if (!k) {
            httpd_resp_sendstr_chunk(req, p);
            return;
        }
        httpd_resp_send_chunk(req, p, k - p);
        size_t skip = 2; /* unknown "{{": emit it literally */
        const char *out = "{{";
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (strncmp(k, keys[i].key, keys[i].len) == 0) {
                out = vals[i];
                skip = keys[i].len;
                break;
            }
        }
        httpd_resp_sendstr_chunk(req, out);
        p = k + skip;
    }
}

static esp_err_t handle_root(httpd_req_t *req)
{
    uint16_t pa = provisioning_get_universe();
    char net[8], sub[8], uni[8];
    snprintf(net, sizeof(net), "%u", (pa >> 8) & 0x7F);
    snprintf(sub, sizeof(sub), "%u", (pa >> 4) & 0x0F);
    snprintf(uni, sizeof(uni), "%u", pa & 0x0F);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    send_portal_page(req, net, sub, uni, esp_app_get_description()->version);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Escape " and \ so SSID values are safe inside a JSON string */
static void json_escape(const char *src, char *dst, size_t dstsz)
{
    size_t di = 0;
    for (; *src && di + 2 < dstsz; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20) continue;
        if ((c == '"' || c == '\\') && di + 3 < dstsz) dst[di++] = '\\';
        dst[di++] = (char)c;
    }
    dst[di] = '\0';
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 24) count = 24;

    wifi_ap_record_t *recs = count ? malloc(count * sizeof(*recs)) : NULL;
    if (count && !recs) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    if (count) esp_wifi_scan_get_ap_records(&count, recs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < count; i++) {
        char esc[72], entry[128];
        json_escape((char *)recs[i].ssid, esc, sizeof(esc));
        int n = snprintf(entry, sizeof(entry),
                         "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                         i ? "," : "", esc, recs[i].rssi);
        httpd_resp_send_chunk(req, entry, n);
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    free(recs);
    return ESP_OK;
}

/* URL-decode a %-encoded string in-place */
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static esp_err_t handle_save(httpd_req_t *req)
{
    /* URL-encoded form: every character may take three bytes ("%XX") */
    char body[3 * (SSID_MAX_LEN + PASS_MAX_LEN) + 96];
    if (http_recv_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or too long");
        return ESP_FAIL;
    }

    char ssid[3 * SSID_MAX_LEN + 1] = {0}, pass[3 * PASS_MAX_LEN + 1] = {0};
    char net_str[8] = {0}, sub_str[8] = {0}, uni_str[8] = {0};
    esp_err_t e_ssid = httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    esp_err_t e_pass = httpd_query_key_value(body, "pass", pass, sizeof(pass));
    httpd_query_key_value(body, "anet", net_str, sizeof(net_str));
    httpd_query_key_value(body, "asub", sub_str, sizeof(sub_str));
    httpd_query_key_value(body, "auni", uni_str, sizeof(uni_str));
    url_decode(ssid);
    url_decode(pass);

    if (e_ssid != ESP_OK || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
        return ESP_FAIL;
    }
    /* Reject instead of silently truncating: a cut-off password never connects */
    if (strlen(ssid) > SSID_MAX_LEN || strlen(pass) > PASS_MAX_LEN ||
        e_pass == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID or password too long");
        return ESP_FAIL;
    }

    /* Port-address = net (0-127), sub-net (0-15), universe (0-15) */
    long net = net_str[0] ? strtol(net_str, NULL, 10) : 0;
    long sub = sub_str[0] ? strtol(sub_str, NULL, 10) : 0;
    long uni = uni_str[0] ? strtol(uni_str, NULL, 10) : 0;
    if (net < 0) net = 0;
    if (net > 127) net = 127;
    if (sub < 0) sub = 0;
    if (sub > 15) sub = 15;
    if (uni < 0) uni = 0;
    if (uni > 15) uni = 15;
    long univ = (net << 8) | (sub << 4) | uni;

    const char *resp_html =
        "<html><body><h2>Saved!</h2>"
        "<p>The Art-Net node is now connecting to the WiFi network.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_html, HTTPD_RESP_USE_STRLEN);

    save_credentials(ssid, pass);
    provisioning_save_universe((uint16_t)univ);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* Catch-all 404 → redirect unknown paths/hosts to the portal */
static esp_err_t handle_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_portal(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    if (httpd_start(&s_server, &cfg) != ESP_OK) return;

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = handle_root };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = handle_save };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET,  .handler = handle_scan };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
    httpd_register_uri_handler(s_server, &scan);

    /* Redirect any unrecognised path → portal (iOS/Android captive detection) */
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_redirect);

    ESP_LOGI(TAG, "Captive portal running at http://" PORTAL_IP);
}

/* ── SoftAP mode ──────────────────────────────────────────────────────── */

static void start_ap_mode(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[20];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X%02X",
             AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ssid);

    /* APSTA: AP serves the portal, STA interface is used for channel scanning */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    ESP_LOGI(TAG, "SoftAP started — SSID: \"%s\"  → open http://" PORTAL_IP, ssid);
    start_portal();
    xTaskCreate(dns_task, "dns", 3072, NULL, 5, NULL);
}

/* ── STA mode: connect with stored credentials ────────────────────────── */

static bool try_sta(const char *ssid, const char *pass)
{
    /* The fields are not strings: a 32-character SSID or a 64-character key
       fills them completely, without a terminator. */
    wifi_config_t sta_cfg = {0};
    memcpy(sta_cfg.sta.ssid,     ssid, strnlen(ssid, sizeof(sta_cfg.sta.ssid)));
    memcpy(sta_cfg.sta.password, pass, strnlen(pass, sizeof(sta_cfg.sta.password)));

    s_sta_connecting = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();
    /* Modem sleep delays and bunches incoming Art-Net packets: keep the radio on */
    esp_wifi_set_ps(WIFI_PS_NONE);

    EventBits_t bits = xEventGroupWaitBits(s_eg,
                                           WIFI_CONN_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    s_sta_connecting = false;
    s_online = (bits & WIFI_CONN_BIT) != 0;     /* from now on: reconnect forever */
    return s_online;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void provisioning_start(void)
{
    s_eg = xEventGroupCreate();

    /* Without these nothing works: fail loudly (abort + reboot) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, "dmx-artnet");
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    const esp_timer_create_args_t targs = { .callback = reconnect_timer_cb, .name = "wifi_reconn" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    char ssid[SSID_MAX_LEN + 1], pass[PASS_MAX_LEN + 1];
    bool have_creds = load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    if (have_creds) {
        ESP_LOGI(TAG, "Stored SSID: \"%s\" — connecting...", ssid);
        led_set_state(LED_CONNECTING);
        if (try_sta(ssid, pass)) {
            led_set_state(LED_CONNECTED);
            return;
        }

        ESP_LOGW(TAG, "Connection failed, falling back to provisioning");
        esp_wifi_stop();
        xEventGroupClearBits(s_eg, WIFI_CONN_BIT | WIFI_FAIL_BIT);
        s_retry = 0;
    }

    led_set_state(LED_PROVISIONING);
    start_ap_mode();

    /* Stay in the portal until /save restarts the device. If credentials are
       stored (the network was just not reachable at boot, e.g. after a power
       cut), keep retrying in the background and restart once it works. */
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_eg, WIFI_CONN_BIT, pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(FALLBACK_RETRY_MS));
        if (bits & WIFI_CONN_BIT) {
            ESP_LOGI(TAG, "Stored network is back — restarting into normal mode");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        if (!have_creds) continue;

        /* A connection attempt scans other channels and disturbs the access
           point, so leave it alone while somebody is using the portal. */
        wifi_sta_list_t clients;
        if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) continue;

        net_config_t netcfg;
        provisioning_get_net_config(&netcfg);
        if (!netcfg.dhcp) apply_static_ip(&netcfg);
        ESP_LOGI(TAG, "Retrying stored network \"%s\"", ssid);
        esp_wifi_connect();
    }
}
