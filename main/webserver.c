#include "webserver.h"
#include "artnet.h"
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

/* ── Status page HTML ─────────────────────────────────────────────────── */

static const char STATUS_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><title>ArtNet Node</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;max-width:520px;margin:40px auto;padding:0 16px}"
    "h2,h3{margin-bottom:8px}"
    "table{width:100%;border-collapse:collapse;margin-bottom:16px}"
    "td{padding:6px 8px;border-bottom:1px solid #eee}"
    "td:first-child{color:#666;font-size:.9em;width:45%}"
    "#dg{display:grid;grid-template-columns:repeat(16,1fr);gap:3px;margin-bottom:20px}"
    ".bc{text-align:center}"
    ".bo{height:36px;background:#eee;border-radius:2px;position:relative;overflow:hidden}"
    ".bi{position:absolute;bottom:0;width:100%;background:#0070d2;height:0;transition:height .25s}"
    ".bn{font-size:9px;color:#888;margin-top:2px}"
    "hr{border:none;border-top:1px solid #eee;margin:20px 0}"
    "input[type=text],input[type=file]{width:100%;box-sizing:border-box;padding:8px;"
        "margin-bottom:8px;border:1px solid #ccc;border-radius:4px;font-size:1em}"
    "button{padding:10px 20px;border:none;border-radius:4px;font-size:1em;"
        "cursor:pointer;background:#0070d2;color:#fff;margin-bottom:8px}"
    ".s{background:#555}"
    "#msg{margin-top:8px;font-size:.85em}"
    ".ok{color:green}.er{color:red}"
    "</style></head><body>"
    "<h2>ArtNet DMX Node</h2>"
    "<table>"
    "<tr><td>Firmware</td><td id='ver'>&#8211;</td></tr>"
    "<tr><td>IP Address</td><td id='ip'>&#8211;</td></tr>"
    "<tr><td>WiFi RSSI</td><td id='rs'>&#8211;</td></tr>"
    "<tr><td>Art-Net Universe</td><td id='un'>&#8211;</td></tr>"
    "<tr><td>Art-Net</td><td id='pp'>&#8211;</td></tr>"
    "</table>"
    "<h3>DMX Ch 1&#8211;16</h3>"
    "<div id='dg'></div>"
    "<hr>"
    "<h3>Firmware Update</h3>"
    "<p><b>Flash from URL</b></p>"
    "<input type='text' id='url' placeholder='https://&#8230;/firmware.bin'>"
    "<button onclick='fu()'>Flash from URL</button>"
    "<hr style='border-style:dashed'>"
    "<p><b>Upload binary</b></p>"
    "<input type='file' id='bin' accept='.bin'>"
    "<button class='s' onclick='up()'>Upload &amp; flash</button>"
    "<div id='msg'></div>"
    "<script>"
    "var g=document.getElementById('dg');"
    "for(var i=0;i<16;i++){"
      "g.innerHTML+='<div class=bc><div class=bo><div class=bi id=f'+i+'></div></div>"
                    "<div class=bn>'+(i+1)+'</div></div>';"
    "}"
    "function ref(){"
      "fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
        "document.getElementById('ver').textContent=d.version;"
        "document.getElementById('ip').textContent=d.ip;"
        "document.getElementById('rs').textContent=d.rssi+' dBm';"
        "document.getElementById('un').textContent=d.universe;"
        "var p=document.getElementById('pp');"
        "p.textContent=d.artnet_pps+' pkt/s';"
        "p.style.color=d.artnet_pps>0?'green':'';"
        "for(var i=0;i<16&&i<d.dmx.length;i++){"
          "var f=document.getElementById('f'+i);"
          "if(f)f.style.height=(d.dmx[i]/2.55).toFixed(0)+'%';"
        "}"
      "}).catch(function(){});"
    "}"
    "ref();setInterval(ref,2000);"
    "function sm(t,ok){var m=document.getElementById('msg');m.textContent=t;m.className=ok?'ok':'er';}"
    "function fu(){"
      "var u=document.getElementById('url').value.trim();"
      "if(!u){sm('Enter a URL.',0);return;}"
      "sm('Sending…','');"
      "fetch('/ota/url',{method:'POST',"
        "headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({url:u})})"
      ".then(function(r){return r.json();})"
      ".then(function(d){sm(d.msg||(d.ok?'OK':'Error'),d.ok);})"
      ".catch(function(){sm('Request failed.',0);});"
    "}"
    "function up(){"
      "var f=document.getElementById('bin').files[0];"
      "if(!f){sm('Select a .bin file.',0);return;}"
      "sm('Uploading '+f.name+' ('+Math.round(f.size/1024)+' kB)…','');"
      "fetch('/ota/upload',{method:'POST',"
        "headers:{'Content-Type':'application/octet-stream'},"
        "body:f})"
      ".then(function(r){return r.json();})"
      ".then(function(d){sm(d.ok?'Flash OK — restarting…':'Error: '+d.msg,d.ok);})"
      ".catch(function(){sm('Upload failed.',0);});"
    "}"
    "</script></body></html>";

/* ── Helpers ──────────────────────────────────────────────────────────── */

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
    httpd_resp_sendstr(req, STATUS_HTML);
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

    /* DMX channels 1-16 */
    uint8_t dmx[16] = {0};
    artnet_copy_dmx(dmx, sizeof(dmx));

    /* Build DMX array string */
    char dmx_arr[80];
    int pos = 0;
    pos += snprintf(dmx_arr + pos, sizeof(dmx_arr) - pos, "[");
    for (int i = 0; i < 16; i++)
        pos += snprintf(dmx_arr + pos, sizeof(dmx_arr) - pos,
                        "%s%u", i ? "," : "", dmx[i]);
    snprintf(dmx_arr + pos, sizeof(dmx_arr) - pos, "]");

    char json[256];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\","
             "\"ip\":\"%s\","
             "\"rssi\":%d,"
             "\"universe\":%u,"
             "\"artnet_pps\":%u,"
             "\"dmx\":%s}",
             ver, ip_str, rssi,
             (unsigned)provisioning_get_universe(), (unsigned)pps, dmx_arr);

    send_json(req, json);
    return ESP_OK;
}

/* ── POST /ota/url ────────────────────────────────────────────────────── */

static void ota_url_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t err = ota_flash_url(url);
    free(url);
    if (err == ESP_OK) {
        esp_restart();
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
    }

    err = esp_ota_end(ota_handle);
    if (!write_ok || err != ESP_OK) {
        ESP_LOGE(TAG, "OTA upload failed");
        send_json(req, "{\"ok\":false,\"msg\":\"Flash write failed\"}");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        send_json(req, "{\"ok\":false,\"msg\":\"Could not set boot partition\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA upload successful (%u bytes), restarting", (unsigned)req->content_len);
    send_json(req, "{\"ok\":true}");
    xTaskCreate(restart_task, "restart", 1024, NULL, 1, NULL);
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
        { .uri = "/",           .method = HTTP_GET,  .handler = handle_root      },
        { .uri = "/api/status", .method = HTTP_GET,  .handler = handle_status    },
        { .uri = "/ota/url",    .method = HTTP_POST, .handler = handle_ota_url   },
        { .uri = "/ota/upload", .method = HTTP_POST, .handler = handle_ota_upload },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "Status page: http://dmx-artnet.local/");
}
