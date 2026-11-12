#include "webserver.h"
#include "artnet.h"
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

#define GITHUB_FW_URL \
    "https://github.com/thomas-engineering/esp_artnet/releases/latest/download/esp_artnet.bin"

/* ── Status page HTML ─────────────────────────────────────────────────── */

static const char STATUS_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'><title>Art-Net Node</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;max-width:520px;margin:0 auto;padding:16px}"
    "h2{margin-bottom:8px}h3{margin:0 0 10px}"
    "table{width:100%;border-collapse:collapse;margin-bottom:16px}"
    "td{padding:6px 8px;border-bottom:1px solid #eee}"
    "td:first-child{color:#666;font-size:.9em;width:45%}"
    "#dg{display:grid;grid-template-columns:repeat(16,1fr);gap:3px;margin-bottom:20px}"
    "#cw{display:flex;gap:8px;align-items:center;margin-bottom:10px}"
    "#cw button{min-width:48px;margin:0;padding:0 12px}"
    "#cl{flex:1;text-align:center;font-weight:bold}"
    ".bc{text-align:center}"
    ".bo{height:36px;background:#eee;border-radius:2px;position:relative;overflow:hidden}"
    ".bi{position:absolute;bottom:0;width:100%;background:#0070d2;height:0;transition:height .25s}"
    ".bn{font-size:9px;color:#888;margin-top:2px}"
    "hr{border:none;border-top:1px solid #eee;margin:20px 0}"
    "label{display:block;font-size:.9em;margin-bottom:3px}"
    "input[type=text],input[type=number],input[type=file]{"
        "width:100%;box-sizing:border-box;padding:10px 8px;min-height:44px;"
        "margin-bottom:8px;border:1px solid #ccc;border-radius:4px;font-size:1em}"
    "button{min-height:48px;padding:0 20px;border:none;border-radius:4px;font-size:1em;"
        "cursor:pointer;background:#0070d2;color:#fff;margin-bottom:8px}"
    ".s{background:#555}"
    "#msg{margin-top:8px;font-size:.85em}"
    ".ok{color:green}.er{color:red}"
    "#uvw{display:flex;gap:8px;align-items:stretch;margin-bottom:12px}"
    "#uvw input{flex:1;text-align:center;font-size:2em;font-weight:bold;"
        "letter-spacing:.05em;min-height:64px;margin:0;padding:4px}"
    "#uv{-moz-appearance:textfield;appearance:textfield}"
    "#uv::-webkit-inner-spin-button,#uv::-webkit-outer-spin-button{-webkit-appearance:none;margin:0}"
    "#uvw .adj{background:#555;min-width:56px;font-size:1.6em;padding:0;margin:0;flex-shrink:0}"
    "#uvw .sav{min-width:88px;margin:0;flex-shrink:0}"
    ".rl{display:flex;align-items:center;gap:12px;padding:12px 0;"
        "border-bottom:1px solid #f0f0f0;cursor:pointer;margin-bottom:0}"
    ".rl input[type=radio]{width:22px;height:22px;min-height:0;min-width:0;margin:0;flex-shrink:0}"
    ".rl span{font-size:1em}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}"
    ".ind{width:100%!important;animation:pulse 1.4s ease-in-out infinite}"
    "</style></head><body>"
    "<h2>Art-Net DMX Node</h2>"
    "<table>"
    "<tr><td>Firmware</td><td id='ver'>&#8211;</td></tr>"
    "<tr><td>IP Address</td><td id='ip'>&#8211;</td></tr>"
    "<tr><td>WiFi RSSI</td><td id='rs'>&#8211;</td></tr>"
    "<tr><td>Art-Net Universe</td><td id='un'>&#8211;</td></tr>"
    "<tr><td>Art-Net</td><td id='pp'>&#8211;</td></tr>"
    "</table>"
    "<h3>DMX Channels</h3>"
    "<div id='cw'>"
      "<button class='s' type='button' onclick='cj(0)'>&#9198;</button>"
      "<button class='s' type='button' onclick='cs(-16)'>&#9664;</button>"
      "<div id='cl'></div>"
      "<button class='s' type='button' onclick='cs(16)'>&#9654;</button>"
      "<button class='s' type='button' onclick='cj(464)'>&#9197;</button>"
    "</div>"
    "<div id='dg'></div>"
    "<hr>"
    "<h3>Art-Net Settings</h3>"
    "<label for='uv'>Universe (0&#8211;32767)</label>"
    "<div id='uvw'>"
      "<button class='adj' type='button' onclick='ud(-1)'>&#8722;</button>"
      "<input type='number' id='uv' min='0' max='32767' value='0'>"
      "<button class='adj' type='button' onclick='ud(1)'>+</button>"
      "<button class='sav' onclick='su()'>Save</button>"
    "</div>"
    "<hr>"
    "<h3>Network Configuration</h3>"
    "<label class='rl'>"
      "<input type='radio' name='ipmode' id='nd' value='dhcp' checked onchange='nt()'>"
      "<span>DHCP (automatic)</span>"
    "</label>"
    "<label class='rl' style='margin-bottom:12px'>"
      "<input type='radio' name='ipmode' id='ns' value='static' onchange='nt()'>"
      "<span>Static IP</span>"
    "</label>"
    "<div id='sf' style='display:none'>"
      "<label for='sa'>IPv4 Address</label>"
      "<input type='text' id='sa' placeholder='e.g. 192.168.1.100'>"
      "<label for='sk'>Netmask</label>"
      "<input type='text' id='sk' placeholder='e.g. 255.255.255.0'>"
      "<label for='sg'>Gateway</label>"
      "<input type='text' id='sg' placeholder='e.g. 192.168.1.1'>"
      "<label for='sd'>DNS Server</label>"
      "<input type='text' id='sd' placeholder='e.g. 8.8.8.8'>"
    "</div>"
    "<button onclick='sn()'>Save Network</button>"
    "<hr>"
    "<h3>Firmware Update</h3>"
    "<div id='otaprog' style='display:none;margin-bottom:16px'>"
      "<div id='otatxt' style='font-size:.9em;margin-bottom:6px'>Flashing...</div>"
      "<div style='background:#eee;border-radius:4px;height:20px;overflow:hidden'>"
        "<div id='otabar' style='height:100%;width:0;background:#0070d2;transition:width .3s'></div>"
      "</div>"
    "</div>"
    "<p><b>Flash from URL</b></p>"
    "<input type='text' id='url' value='" GITHUB_FW_URL "'>"
    "<button onclick='fu()'>Flash from URL</button>"
    "<hr style='border-style:dashed'>"
    "<p><b>Upload binary</b></p>"
    "<input type='file' id='bin' accept='.bin'>"
    "<button class='s' onclick='up()'>Upload &amp; flash</button>"
    "<div id='msg'></div>"
    "<script>"
    "var g=document.getElementById('dg');"
    "var off=0;"
    "for(var i=0;i<48;i++){"
      "g.innerHTML+='<div class=bc><div class=bo><div class=bi id=f'+i+'></div></div>"
                    "<div class=bn id=n'+i+'></div></div>';"
    "}"
    "function lb(){"
      "for(var i=0;i<48;i++)document.getElementById('n'+i).textContent=off+i+1;"
      "document.getElementById('cl').textContent='Ch '+(off+1)+'\\u2013'+(off+48);"
    "}"
    "function cs(d){"
      "off=Math.max(0,Math.min(512-48,off+d));lb();ref();"
    "}"
    "function cj(o){off=o;lb();ref();}"
    "lb();"
    "var ini=false;"
    "function ref(){"
      "fetch('/api/status?off='+off).then(function(r){return r.json();}).then(function(d){"
        "document.getElementById('ver').textContent=d.version;"
        "document.getElementById('ip').textContent=d.ip;"
        "document.getElementById('rs').textContent=d.rssi+' dBm';"
        "document.getElementById('un').textContent=d.universe;"
        "var p=document.getElementById('pp');"
        "p.textContent=d.artnet_pps+' pkt/s';"
        "p.style.color=d.artnet_pps>0?'green':'';"
        "for(var i=0;i<48&&i<d.dmx.length;i++){"
          "var f=document.getElementById('f'+i);"
          "if(f)f.style.height=(d.dmx[i]/2.55).toFixed(0)+'%';"
        "}"
        "if(!ini){"
          "ini=true;"
          "document.getElementById('uv').value=d.universe;"
          "if(d.net_mode==='static'){"
            "document.getElementById('ns').checked=true;"
            "document.getElementById('sf').style.display='';"
            "document.getElementById('sa').value=d.s_ip||'';"
            "document.getElementById('sk').value=d.s_nm||'';"
            "document.getElementById('sg').value=d.s_gw||'';"
            "document.getElementById('sd').value=d.s_dns||'';"
          "}"
        "}"
      "}).catch(function(){});"
    "}"
    "ref();setInterval(ref,2000);"
    "function sm(t,ok){var m=document.getElementById('msg');m.textContent=t;m.className=ok?'ok':'er';}"
    "function nt(){"
      "document.getElementById('sf').style.display="
        "document.getElementById('ns').checked?'':'none';"
    "}"
    "function ud(d){"
      "var e=document.getElementById('uv');"
      "var v=(parseInt(e.value)||0)+d;"
      "if(v<0)v=0;if(v>32767)v=32767;"
      "e.value=v;"
    "}"
    "function su(){"
      "var u=parseInt(document.getElementById('uv').value);"
      "if(isNaN(u)||u<0||u>32767){sm('Invalid universe.',0);return;}"
      "sm('Saving…','');"
      "fetch('/api/config',{method:'POST',"
        "headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({universe:u})})"
      ".then(function(r){return r.json();})"
      ".then(function(d){sm(d.ok?'Saved!':'Error: '+(d.msg||''),d.ok);})"
      ".catch(function(){sm('Request failed.',0);});"
    "}"
    "function sn(){"
      "var mode=document.querySelector('input[name=ipmode]:checked').value;"
      "var o={net_mode:mode};"
      "if(mode==='static'){"
        "o.ip=document.getElementById('sa').value.trim();"
        "o.nm=document.getElementById('sk').value.trim();"
        "o.gw=document.getElementById('sg').value.trim();"
        "o.dns=document.getElementById('sd').value.trim();"
        "if(!o.ip||!o.nm||!o.gw||!o.dns){sm('Fill in all fields.',0);return;}"
      "}"
      "sm('Saving…','');"
      "fetch('/api/config',{method:'POST',"
        "headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify(o)})"
      ".then(function(r){return r.json();})"
      ".then(function(d){"
        "sm(d.ok?(d.restart?'Saved — restarting…':'Saved!'):'Error: '+(d.msg||''),d.ok);"
        "if(d.ok&&d.restart)setTimeout(function(){location.reload();},5000);"
      "})"
      ".catch(function(){sm('Request failed.',0);});"
    "}"
    "var otaTimer=null;"
    "function startOtaPoll(){"
      "var bar=document.getElementById('otabar');"
      "document.getElementById('otaprog').style.display='';"
      "bar.className='ind';bar.style.width='0';"
      "document.getElementById('otatxt').textContent='Starting...';"
      "if(otaTimer)clearInterval(otaTimer);"
      "otaTimer=setInterval(pollOta,500);"
    "}"
    "function stopOtaPoll(){"
      "if(otaTimer){clearInterval(otaTimer);otaTimer=null;}"
      "document.getElementById('otaprog').style.display='none';"
    "}"
    "function pollOta(){"
      "fetch('/api/ota/progress').then(function(r){return r.json();}).then(function(d){"
        "var bar=document.getElementById('otabar');"
        "var lbl=document.getElementById('otatxt');"
        "if(d.phase==='active'){"
          "lbl.textContent='Flashing... '+d.pct+'%';"
          "if(d.pct>0){bar.className='';bar.style.width=d.pct+'%';}"
        "} else if(d.phase==='rebooting'){"
          "bar.className='';bar.style.width='100%';"
          "lbl.textContent='Done — rebooting...';"
          "if(otaTimer){clearInterval(otaTimer);otaTimer=null;}"
          "setTimeout(function(){location.reload();},10000);"
        "} else if(d.phase==='error'){"
          "stopOtaPoll();"
          "sm('Error: '+(d.msg||'OTA failed'),0);"
        "}"
      "}).catch(function(){});"
    "}"
    "function fu(){"
      "var u=document.getElementById('url').value.trim();"
      "if(!u){sm('Enter a URL.',0);return;}"
      "startOtaPoll();"
      "fetch('/ota/url',{method:'POST',"
        "headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({url:u})})"
      ".then(function(r){return r.json();})"
      ".then(function(d){if(!d.ok){stopOtaPoll();sm(d.msg||'Error',0);}})"
      ".catch(function(){stopOtaPoll();sm('Request failed.',0);});"
    "}"
    "function up(){"
      "var f=document.getElementById('bin').files[0];"
      "if(!f){sm('Select a .bin file.',0);return;}"
      "startOtaPoll();"
      "fetch('/ota/upload',{method:'POST',"
        "headers:{'Content-Type':'application/octet-stream'},"
        "body:f})"
      ".then(function(r){return r.json();})"
      ".then(function(d){if(!d.ok){stopOtaPoll();sm('Error: '+d.msg,0);}})"
      ".catch(function(){stopOtaPoll();sm('Upload failed.',0);});"
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

    char json[768];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\","
             "\"ip\":\"%s\","
             "\"rssi\":%d,"
             "\"universe\":%u,"
             "\"artnet_pps\":%u,"
             "\"dmx\":%s,"
             "\"net_mode\":\"%s\","
             "\"s_ip\":\"%s\","
             "\"s_nm\":\"%s\","
             "\"s_gw\":\"%s\","
             "\"s_dns\":\"%s\"}",
             ver, ip_str, rssi,
             (unsigned)provisioning_get_universe(), (unsigned)pps, dmx_arr,
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
        xTaskCreate(restart_task, "restart", 1024, NULL, 1, NULL);
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
