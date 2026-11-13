#include "ota.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "nvs_flash.h"

#define GITHUB_OWNER        "thomas-engineering"
#define GITHUB_REPO         "esp_artnet"
#define GITHUB_API_URL      "https://api.github.com/repos/" \
                            GITHUB_OWNER "/" GITHUB_REPO "/releases/latest"
#define OTA_CHECK_INTERVAL_S  (6 * 60 * 60)   /* 6 hours */
#define HTTP_RESP_BUF_SIZE    4096
/* GitHub redirects to long signed URLs; the default 512 B header buffer overflows */
#define OTA_HTTP_BUF_SIZE     4096

#define OTA_NVS_NAMESPACE     "ota"
#define OTA_NVS_KEY_AUTO      "auto"

static const char *TAG = "ota";

static volatile bool s_auto_enabled = false;   /* opt-in via the web UI */

bool ota_get_auto_enabled(void)
{
    return s_auto_enabled;
}

void ota_set_auto_enabled(bool enabled)
{
    s_auto_enabled = enabled;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, OTA_NVS_KEY_AUTO, enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Automatic OTA %s", enabled ? "enabled" : "disabled");
}

static void load_auto_enabled(void)
{
    nvs_handle_t h;
    uint8_t v = 0; /* default: disabled until switched on in the web UI */
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, OTA_NVS_KEY_AUTO, &v);
        nvs_close(h);
    }
    s_auto_enabled = (v != 0);
}

/* Accumulate HTTP response body */
static char  s_resp[HTTP_RESP_BUF_SIZE];
static int   s_resp_len;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int remaining = (int)sizeof(s_resp) - s_resp_len - 1;
        if (remaining > 0) {
            int copy = evt->data_len < remaining ? evt->data_len : remaining;
            memcpy(s_resp + s_resp_len, evt->data, copy);
            s_resp_len += copy;
            s_resp[s_resp_len] = '\0';
        }
    }
    return ESP_OK;
}

/* Compares "major.minor.patch" strings; returns >0 if a is newer than b. */
static int version_cmp(const char *a, const char *b)
{
    int va[3] = {0, 0, 0}, vb[3] = {0, 0, 0};
    sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) {
            return va[i] > vb[i] ? 1 : -1;
        }
    }
    return 0;
}

static void check_and_update(void)
{
    s_resp_len = 0;
    s_resp[0]  = '\0';

    esp_http_client_config_t http_cfg = {
        .url              = GITHUB_API_URL,
        .event_handler    = http_evt,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent       = "esp-artnet-ota/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GitHub API request failed: %s", esp_err_to_name(err));
        return;
    }

    cJSON *root = cJSON_Parse(s_resp);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        return;
    }

    cJSON *tag    = cJSON_GetObjectItem(root, "tag_name");
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    if (!cJSON_IsString(tag) || !cJSON_IsArray(assets)) {
        ESP_LOGW(TAG, "Unexpected API response (rate-limit hit?)");
        cJSON_Delete(root);
        return;
    }

    /* GitHub tags are "v0.0.2", the app version is "0.0.2": ignore the prefix */
    const char *tag_ver = tag->valuestring;
    if (tag_ver[0] == 'v' || tag_ver[0] == 'V') {
        tag_ver++;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    if (version_cmp(tag_ver, desc->version) <= 0) {
        ESP_LOGI(TAG, "Firmware up to date (%s, latest release %s)", desc->version, tag_ver);
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "New release: %s  (current: %s)", tag->valuestring, desc->version);

    /* Find the first .bin asset */
    char *fw_url = NULL;
    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        cJSON *name = cJSON_GetObjectItem(asset, "name");
        cJSON *url  = cJSON_GetObjectItem(asset, "browser_download_url");
        if (cJSON_IsString(name) && cJSON_IsString(url)
            && strstr(name->valuestring, ".bin")) {
            fw_url = strdup(url->valuestring);
            break;
        }
    }
    cJSON_Delete(root);

    if (!fw_url) {
        ESP_LOGE(TAG, "No .bin asset in release");
        return;
    }

    ESP_LOGI(TAG, "Downloading firmware from: %s", fw_url);
    esp_http_client_config_t ota_http = {
        .url               = fw_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size       = OTA_HTTP_BUF_SIZE,
        .buffer_size_tx    = OTA_HTTP_BUF_SIZE,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &ota_http };
    err = esp_https_ota(&ota_cfg);
    free(fw_url);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful — restarting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
}

esp_err_t ota_flash_url(const char *url, ota_progress_cb_t cb)
{
    ESP_LOGI(TAG, "Flashing from URL: %s", url);
    esp_http_client_config_t http_cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size       = OTA_HTTP_BUF_SIZE,
        .buffer_size_tx    = OTA_HTTP_BUF_SIZE,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            if (cb) {
                int read  = esp_https_ota_get_image_len_read(handle);
                int total = esp_https_ota_get_image_size(handle);
                cb(total > 0 ? read * 100 / total : 0);
            }
        } else {
            break;
        }
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA incomplete — aborting");
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return err;
    }

    err = esp_https_ota_finish(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA from URL successful");
        if (cb) cb(100);
    } else {
        ESP_LOGE(TAG, "esp_https_ota_finish: %s", esp_err_to_name(err));
    }
    return err;
}

static void ota_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(10000)); /* 10 s startup grace period */
    while (1) {
        if (s_auto_enabled) {
            check_and_update();
        } else {
            ESP_LOGI(TAG, "Automatic OTA disabled — skipping check");
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)OTA_CHECK_INTERVAL_S * 1000));
    }
}

void ota_start(void)
{
    load_auto_enabled();
    xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL);
}
