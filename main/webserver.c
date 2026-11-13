#include "webserver.h"
#include "artnet.h"
#include "artnet_poll.h"
#include "dmx.h"
#include "ota.h"
#include "provisioning.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include <inttypes.h>

static const char *TAG = "webserver";

typedef enum {
    OTA_PHASE_IDLE      = 0,
    OTA_PHASE_ACTIVE    = 1,
    OTA_PHASE_REBOOTING = 2,
    OTA_PHASE_ERROR     = 3,
} ota_phase_t;

static volatile ota_phase_t s_ota_phase  = OTA_PHASE_IDLE;
static volatile int         s_ota_pct    = 0;
static char                 s_ota_errmsg[64] = {0};

/* ── Status page HTML ─────────────────────────────────────────────────── */

/* Status page: web/status.html, embedded at build time (EMBED_TXTFILES) */
extern const char status_html_start[] asm("_binary_status_html_start");

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Escape " and \ so text is safe inside a JSON string (names are plain ASCII) */
static void json_escape(const char *src, char *dst, size_t dstsz)
{
    size_t di = 0;
    for (; *src && di + 3 < dstsz; src++) {
        if (*src == '"' || *src == '\\') dst[di++] = '\\';
        dst[di++] = *src;
    }
    dst[di] = '\0';
}

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

/* ── GET / ────────────────────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, status_html_start);
    return ESP_OK;
}

/* ── GET /api/status ──────────────────────────────────────────────────── */

static uint32_t s_last_pkt  = 0;
static int64_t  s_last_us   = 0;

static esp_err_t handle_status(httpd_req_t *req)
{
    /* Version */
    const char *ver = esp_app_get_description()->version;

    /* IP address */
    char ip_str[16] = "?.?.?.?";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK)
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&info.ip));
    }

    /* RSSI */
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    /* Art-Net pps */
    uint32_t now_pkt = artnet_get_pkt_count();
    int64_t  now_us  = esp_timer_get_time();
    uint32_t pps     = 0;
    if (s_last_us > 0) {
        float dt = (now_us - s_last_us) / 1e6f;
        if (dt > 0.0f) pps = (uint32_t)((now_pkt - s_last_pkt) / dt);
    }
    s_last_pkt = now_pkt;
    s_last_us  = now_us;

    /* Requested window of 48 DMX channels (3 rows of 16), start offset via ?off= */
    int off = 0;
    char q[32], ov[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "off", ov, sizeof(ov)) == ESP_OK) {
        off = atoi(ov);
    }
    if (off < 0) off = 0;
    if (off > DMX_CHANNELS - 48) off = DMX_CHANNELS - 48;
    off -= off % 16;

    uint8_t all[DMX_CHANNELS] = {0};
    artnet_copy_dmx(all, sizeof(all));
    const uint8_t *dmx = all + off;

    /* Build DMX array string */
    char dmx_arr[200];
    int pos = 0;
    pos += snprintf(dmx_arr + pos, sizeof(dmx_arr) - pos, "[");
    for (int i = 0; i < 48; i++)
        pos += snprintf(dmx_arr + pos, sizeof(dmx_arr) - pos,
                        "%s%u", i ? "," : "", dmx[i]);
    snprintf(dmx_arr + pos, sizeof(dmx_arr) - pos, "]");

    /* Network config */
    net_config_t netcfg;
    provisioning_get_net_config(&netcfg);

    char nshort[ARTNET_SHORT_NAME_LEN], nlong[ARTNET_LONG_NAME_LEN];
    char eshort[2 * ARTNET_SHORT_NAME_LEN], elong[2 * ARTNET_LONG_NAME_LEN];
    artnet_names_get(nshort, sizeof(nshort), nlong, sizeof(nlong));
    json_escape(nshort, eshort, sizeof(eshort));
    json_escape(nlong,  elong,  sizeof(elong));

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\","
             "\"ip\":\"%s\","
             "\"rssi\":%d,"
             "\"universe\":%u,"
             "\"artnet_pps\":%u,"
             "\"dmx\":%s,"
             "\"auto_ota\":%s,"
             "\"name_short\":\"%s\","
             "\"name_long\":\"%s\","
             "\"net_mode\":\"%s\","
             "\"s_ip\":\"%s\","
             "\"s_nm\":\"%s\","
             "\"s_gw\":\"%s\","
             "\"s_dns\":\"%s\"}",
             ver, ip_str, rssi,
             (unsigned)provisioning_get_universe(), (unsigned)pps, dmx_arr,
             ota_get_auto_enabled() ? "true" : "false",
             eshort, elong,
             netcfg.dhcp ? "dhcp" : "static",
             netcfg.ip, netcfg.netmask, netcfg.gateway, netcfg.dns);

    send_json(req, json);
    return ESP_OK;
}

/* ── POST /api/config ─────────────────────────────────────────────────── */

static esp_err_t handle_config(httpd_req_t *req)
{
    char body[512] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        send_json(req, "{\"ok\":false,\"msg\":\"Empty body\"}");
        return ESP_OK;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        send_json(req, "{\"ok\":false,\"msg\":\"JSON parse error\"}");
        return ESP_OK;
    }

    bool restart = false;

    cJSON *univ_item = cJSON_GetObjectItem(root, "universe");
    if (cJSON_IsNumber(univ_item)) {
        int u = (int)univ_item->valuedouble;
        if (u < 0)     u = 0;
        if (u > 32767) u = 32767;
        provisioning_save_universe((uint16_t)u);
        artnet_set_universe((uint16_t)u);
        ESP_LOGI(TAG, "Universe set to %d", u);
    }

    /* Art-Net node names (empty string = reset to default) */
    cJSON *ns_item = cJSON_GetObjectItem(root, "name_short");
    cJSON *nl_item = cJSON_GetObjectItem(root, "name_long");
    if (cJSON_IsString(ns_item) || cJSON_IsString(nl_item)) {
        if (artnet_names_update(cJSON_IsString(ns_item) ? ns_item->valuestring : NULL,
                                cJSON_IsString(nl_item) ? nl_item->valuestring : NULL)) {
            artnet_announce();
            ESP_LOGI(TAG, "Art-Net names updated");
        }
    }

    cJSON *auto_item = cJSON_GetObjectItem(root, "auto_ota");
    if (cJSON_IsBool(auto_item)) {
        ota_set_auto_enabled(cJSON_IsTrue(auto_item));
    }

    cJSON *mode_item = cJSON_GetObjectItem(root, "net_mode");
    if (cJSON_IsString(mode_item)) {
        net_config_t cfg = {0};
        cfg.dhcp = (strcmp(mode_item->valuestring, "dhcp") == 0);
        if (!cfg.dhcp) {
            cJSON *ip  = cJSON_GetObjectItem(root, "ip");
            cJSON *nm  = cJSON_GetObjectItem(root, "nm");
            cJSON *gw  = cJSON_GetObjectItem(root, "gw");
            cJSON *dns = cJSON_GetObjectItem(root, "dns");
            if (!cJSON_IsString(ip) || !cJSON_IsString(nm) ||
                !cJSON_IsString(gw) || !cJSON_IsString(dns)) {
                cJSON_Delete(root);
                send_json(req, "{\"ok\":false,\"msg\":\"Missing IP fields\"}");
                return ESP_OK;
            }
            strlcpy(cfg.ip,      ip->valuestring,  sizeof(cfg.ip));
            strlcpy(cfg.netmask, nm->valuestring,  sizeof(cfg.netmask));
            strlcpy(cfg.gateway, gw->valuestring,  sizeof(cfg.gateway));
            strlcpy(cfg.dns,     dns->valuestring, sizeof(cfg.dns));
        }
        provisioning_save_net_config(&cfg);
        restart = true;
        ESP_LOGI(TAG, "Network mode set to %s", cfg.dhcp ? "dhcp" : "static");
    }

    cJSON_Delete(root);

    if (restart) {
        send_json(req, "{\"ok\":true,\"restart\":true}");
        xTaskCreate(restart_task, "restart", 3072, NULL, 1, NULL);
    } else {
        send_json(req, "{\"ok\":true,\"restart\":false}");
    }
    return ESP_OK;
}

/* ── GET /api/ota/progress ────────────────────────────────────────────── */

static esp_err_t handle_ota_progress(httpd_req_t *req)
{
    char json[128];
    switch (s_ota_phase) {
    case OTA_PHASE_ACTIVE:
        snprintf(json, sizeof(json), "{\"phase\":\"active\",\"pct\":%d}", s_ota_pct);
        break;
    case OTA_PHASE_REBOOTING:
        snprintf(json, sizeof(json), "{\"phase\":\"rebooting\"}");
        break;
    case OTA_PHASE_ERROR:
        snprintf(json, sizeof(json), "{\"phase\":\"error\",\"msg\":\"%s\"}", s_ota_errmsg);
        s_ota_phase = OTA_PHASE_IDLE; /* reset so user can retry */
        break;
    default:
        snprintf(json, sizeof(json), "{\"phase\":\"idle\"}");
        break;
    }
    send_json(req, json);
    return ESP_OK;
}

/* ── POST /ota/url ────────────────────────────────────────────────────── */

static void url_ota_progress_cb(int pct)
{
    s_ota_pct = pct;
}

static void ota_url_task(void *arg)
{
    char *url = (char *)arg;
    s_ota_phase = OTA_PHASE_ACTIVE;
    s_ota_pct   = 0;
    esp_err_t err = ota_flash_url(url, url_ota_progress_cb);
    free(url);
    if (err == ESP_OK) {
        s_ota_phase = OTA_PHASE_REBOOTING;
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        snprintf(s_ota_errmsg, sizeof(s_ota_errmsg), "%s", esp_err_to_name(err));
        s_ota_phase = OTA_PHASE_ERROR;
    }
    vTaskDelete(NULL);
}

static esp_err_t handle_ota_url(httpd_req_t *req)
{
    char body[512] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        send_json(req, "{\"ok\":false,\"msg\":\"Empty body\"}");
        return ESP_OK;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    cJSON *url_item = root ? cJSON_GetObjectItem(root, "url") : NULL;
    if (!cJSON_IsString(url_item) || url_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        send_json(req, "{\"ok\":false,\"msg\":\"Missing url field\"}");
        return ESP_OK;
    }

    char *url = strdup(url_item->valuestring);
    cJSON_Delete(root);
    if (!url) {
        send_json(req, "{\"ok\":false,\"msg\":\"OOM\"}");
        return ESP_OK;
    }

    send_json(req, "{\"ok\":true,\"msg\":\"Flashing firmware, device will restart...\"}");
    xTaskCreate(ota_url_task, "ota_url", 8192, url, 4, NULL);
    return ESP_OK;
}

/* ── POST /ota/upload ─────────────────────────────────────────────────── */

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (req->content_len == 0) {
        send_json(req, "{\"ok\":false,\"msg\":\"No data\"}");
        return ESP_OK;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        send_json(req, "{\"ok\":false,\"msg\":\"No OTA partition\"}");
        return ESP_OK;
    }
    if (req->content_len > part->size) {
        send_json(req, "{\"ok\":false,\"msg\":\"File too large for OTA partition\"}");
        return ESP_OK;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_json(req, "{\"ok\":false,\"msg\":\"OTA begin failed\"}");
        return ESP_OK;
    }

    char   buf[512];
    size_t remaining = req->content_len;
    bool   write_ok  = true;

    s_ota_phase = OTA_PHASE_ACTIVE;
    s_ota_pct   = 0;

    while (remaining > 0 && write_ok) {
        size_t to_recv = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            ESP_LOGE(TAG, "recv error during upload");
            write_ok = false;
            break;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            write_ok = false;
            break;
        }
        remaining -= received;
        s_ota_pct = (int)((req->content_len - remaining) * 100 / req->content_len);
    }

    err = esp_ota_end(ota_handle);
    if (!write_ok || err != ESP_OK) {
        ESP_LOGE(TAG, "OTA upload failed");
        strlcpy(s_ota_errmsg, "Flash write failed", sizeof(s_ota_errmsg));
        s_ota_phase = OTA_PHASE_ERROR;
        send_json(req, "{\"ok\":false,\"msg\":\"Flash write failed\"}");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        strlcpy(s_ota_errmsg, "Could not set boot partition", sizeof(s_ota_errmsg));
        s_ota_phase = OTA_PHASE_ERROR;
        send_json(req, "{\"ok\":false,\"msg\":\"Could not set boot partition\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA upload successful (%u bytes), restarting", (unsigned)req->content_len);
    s_ota_phase = OTA_PHASE_REBOOTING;
    send_json(req, "{\"ok\":true}");
    xTaskCreate(restart_task, "restart", 3072, NULL, 1, NULL);
    return ESP_OK;
}

/* ── Start ────────────────────────────────────────────────────────────── */

void webserver_start(void)
{
    httpd_config_t cfg     = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers   = 8;
    cfg.stack_size         = 8192;
    cfg.recv_wait_timeout  = 30;   /* seconds — allows slow firmware uploads */

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                 .method = HTTP_GET,  .handler = handle_root         },
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = handle_status       },
        { .uri = "/api/config",       .method = HTTP_POST, .handler = handle_config       },
        { .uri = "/api/ota/progress", .method = HTTP_GET,  .handler = handle_ota_progress },
        { .uri = "/ota/url",          .method = HTTP_POST, .handler = handle_ota_url      },
        { .uri = "/ota/upload",       .method = HTTP_POST, .handler = handle_ota_upload   },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "Status page: http://dmx-artnet.local/");
}
