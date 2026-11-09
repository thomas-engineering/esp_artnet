#include "provisioning.h"
#include "led.h"
#include <string.h>
#include <stdlib.h>
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
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

static const char *TAG = "prov";
static EventGroupHandle_t s_eg;
static int  s_retry = 0;
static bool s_sta_connecting = false;
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_sta_netif = NULL;

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
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
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
        if (!s_sta_connecting) return;
        if (s_retry < MAX_RETRIES) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "Reconnecting (%d/%d)...", s_retry, MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Connected — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
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

        /* Build response: copy query, adjust header flags, ANCOUNT=1 */
        memcpy(resp, buf, n);
        resp[2] = 0x81;        /* QR=1, AA=1 */
        resp[3] = 0x80;        /* RA=1, RCODE=0 */
        resp[6] = 0x00;        /* ANCOUNT hi */
        resp[7] = 0x01;        /* ANCOUNT lo */

        /* Append A-record answer (name = compressed ptr to offset 12) */
        int rlen = n;
        resp[rlen++] = 0xc0; resp[rlen++] = 0x0c; /* NAME ptr */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* TYPE  = A */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* CLASS = IN */
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3c;  /* TTL   = 60 s */
        resp[rlen++] = 0x00; resp[rlen++] = 0x04;  /* RDLEN = 4 */
        memcpy(resp + rlen, PORTAL_A, 4); rlen += 4;

        sendto(sock, resp, rlen, 0, (struct sockaddr *)&client, clen);
    }
}

/* ── Portal HTML ──────────────────────────────────────────────────────── */

/* HTML split around the dynamically injected universe <input>.
   All visible strings are set by the bilingual JS block in PORTAL_HTML_2. */
static const char PORTAL_HTML_1[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>ArtNet Node Setup</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px}"
    "h2{margin-bottom:4px}"
    "label{display:block;font-size:.9em;margin-bottom:2px}"
    "input,select{width:100%;box-sizing:border-box;padding:9px 8px;"
    "margin-bottom:14px;border:1px solid #ccc;border-radius:4px;font-size:1em}"
    "button{width:100%;padding:11px;border:none;border-radius:4px;"
    "font-size:1em;cursor:pointer;margin-bottom:8px}"
    ".primary{background:#0070d2;color:#fff}"
    ".scan{background:#555;color:#fff}"
    "#net{display:none}"
    ".hint{font-size:.8em;color:#666;margin-top:16px}"
    ".lb{float:right;font-size:.8em;background:none;border:1px solid #0070d2;"
    "color:#0070d2;border-radius:3px;padding:2px 7px;cursor:pointer;margin-top:6px}"
    ".pw-wrap{position:relative}"
    ".pw-wrap input{padding-right:40px}"
    ".eye{position:absolute;right:0;top:0;bottom:0;width:38px;background:none;"
    "border:none;cursor:pointer;padding:7px;color:#888;margin:0}"
    ".eye svg{width:18px;height:18px;display:block}"
    "</style></head><body>"
    "<button class='lb' onclick='tl()' id='lb'></button>"
    "<h2 id='ht'></h2>"
    "<button class='scan' id='sb' type='button' onclick='sc()'></button>"
    "<select id='net' onchange='pk(this)'>"
    "<option value='' id='op'></option>"
    "</select>"
    "<form action='/save' method='post'>"
    "<label id='l1'></label>"
    "<input id='ssid' name='ssid' type='text' autocomplete='off' required>"
    "<label id='l2'></label>"
    "<div class='pw-wrap'>"
    "<input name='pass' type='password' id='pi' autocomplete='current-password'>"
    "<button type='button' class='eye' onclick='ey()'>"
    "<svg id='eo' viewBox='0 0 24 24' fill='none' stroke='currentColor'"
        " stroke-width='2' stroke-linecap='round'>"
    "<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8'/>"
    "<circle cx='12' cy='12' r='3'/>"
    "</svg>"
    "<svg id='ec' viewBox='0 0 24 24' fill='none' stroke='currentColor'"
        " stroke-width='2' stroke-linecap='round' style='display:none'>"
    "<path d='M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8"
            "a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4"
            "c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19"
            "m-6.72-1.07a3 3 0 1 1-4.24-4.24'/>"
    "<line x1='1' y1='1' x2='23' y2='23'/>"
    "</svg>"
    "</button></div>"
    "<label id='l3'></label>";
    /* universe <input> injected here with the stored value */

static const char PORTAL_HTML_2[] =
    "<button class='primary' type='submit' id='l4'></button>"
    "</form>"
    "<p class='hint' id='l5'></p>"
    "<script>"
    "var S={"
      "en:{lb:'DE',ht:'ArtNet Node WiFi Setup',"
          "sb:'Scan networks',sg:'Scanning…',sr:'Scan again',se:'Error — retry',"
          "op:'— Select network —',"
          "l1:'WiFi Name (SSID)',"
          "l2:'Password (leave empty for open network)',"
          "l3:'Art-Net Universe (0 = first universe)',"
          "l4:'Save & connect',"
          "l5:'After saving the node connects to the WiFi and is reachable at its IP address.'},"
      "de:{lb:'EN',ht:'ArtNet Node WLAN-Setup',"
          "sb:'Netzwerke suchen',sg:'Suche…',sr:'Erneut suchen',se:'Fehler — nochmals',"
          "op:'— Netzwerk auswählen —',"
          "l1:'WLAN-Name (SSID)',"
          "l2:'Passwort (leer lassen wenn offen)',"
          "l3:'Art-Net Universe (0 = erstes Universum)',"
          "l4:'Speichern & verbinden',"
          "l5:'Nach dem Speichern verbindet sich der Node mit dem WLAN und ist unter seiner IP-Adresse erreichbar.'}"
    "};"
    "var L=localStorage.getItem('lang')||(navigator.language.toLowerCase().startsWith('de')?'de':'en');"
    "function ap(){"
      "var t=S[L];"
      "['lb','ht','op','l1','l2','l3','l4','l5'].forEach(function(k){"
        "document.getElementById(k).textContent=t[k];});"
      "var b=document.getElementById('sb');"
      "if(!b.disabled)b.textContent=t.sb;"
    "}"
    "function tl(){L=L==='en'?'de':'en';localStorage.setItem('lang',L);ap();}"
    "ap();"
    "function sc(){"
      "var b=document.getElementById('sb');"
      "b.disabled=1;b.textContent=S[L].sg;"
      "fetch('/scan').then(r=>r.json()).then(nets=>{"
        "var s=document.getElementById('net');"
        "s.innerHTML='<option value=\"\">'+S[L].op+'</option>';"
        "nets.forEach(n=>{"
          "var o=document.createElement('option');"
          "o.value=n.ssid;"
          "o.textContent=n.ssid+' ('+n.rssi+' dBm)';"
          "s.appendChild(o);});"
        "s.style.display='block';"
        "b.disabled=0;b.textContent=S[L].sr;"
      "}).catch(()=>{b.disabled=0;b.textContent=S[L].se;});}"
    "function pk(s){if(s.value)document.getElementById('ssid').value=s.value;}"
    "function ey(){"
      "var i=document.getElementById('pi'),"
          "eo=document.getElementById('eo'),"
          "ec=document.getElementById('ec');"
      "if(i.type==='password'){"
        "i.type='text';eo.style.display='none';ec.style.display='';"
      "}else{"
        "i.type='password';eo.style.display='';ec.style.display='none';"
      "}"
    "}"
    "</script>";

/* ── HTTP handlers ────────────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
{
    char univ_input[80];
    snprintf(univ_input, sizeof(univ_input),
             "<input name='univ' type='number' min='0' max='32767' value='%u'>",
             provisioning_get_universe());
    char ver_footer[96];
    snprintf(ver_footer, sizeof(ver_footer),
             "<p class='hint' style='text-align:center'>%s</p></body></html>",
             esp_app_get_description()->version);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PORTAL_HTML_1);
    httpd_resp_sendstr_chunk(req, univ_input);
    httpd_resp_sendstr_chunk(req, PORTAL_HTML_2);
    httpd_resp_sendstr_chunk(req, ver_footer);
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
    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_FAIL;
    }

    char ssid[64] = {0}, pass[64] = {0}, univ_str[8] = {0};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    httpd_query_key_value(body, "univ", univ_str, sizeof(univ_str));
    url_decode(ssid);
    url_decode(pass);
    url_decode(univ_str);

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
        return ESP_FAIL;
    }

    long univ = univ_str[0] ? strtol(univ_str, NULL, 10) : 0;
    if (univ < 0)     univ = 0;
    if (univ > 32767) univ = 32767;

    const char *resp_html =
        "<html><body><h2>Saved!</h2>"
        "<p>The ArtNet node is now connecting to the WiFi network.</p></body></html>";
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
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));

    s_sta_connecting = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();

    EventBits_t bits = xEventGroupWaitBits(s_eg,
                                           WIFI_CONN_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    s_sta_connecting = false;
    return (bits & WIFI_CONN_BIT) != 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void provisioning_start(void)
{
    s_eg = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, "dmx-artnet");
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wcfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    char ssid[64], pass[64];
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
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

    /* Block until /save restarts the device */
    xEventGroupWaitBits(s_eg, WIFI_CONN_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}
