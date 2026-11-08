#include "ota.h"
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

#define GITHUB_OWNER        "thomas-engineering"
#define GITHUB_REPO         "esp_artnet"
#define GITHUB_API_URL      "https://api.github.com/repos/" \
                            GITHUB_OWNER "/" GITHUB_REPO "/releases/latest"
#define OTA_CHECK_INTERVAL_S  (6 * 60 * 60)   /* 6 hours */
#define HTTP_RESP_BUF_SIZE    4096

static const char *TAG = "ota";

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

    const esp_app_desc_t *desc = esp_app_get_description();
    if (strcmp(tag->valuestring, desc->version) == 0) {
        ESP_LOGI(TAG, "Firmware up to date (%s)", desc->version);
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

static void ota_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(10000)); /* 10 s startup grace period */
    while (1) {
        check_and_update();
        vTaskDelay(pdMS_TO_TICKS((uint32_t)OTA_CHECK_INTERVAL_S * 1000));
    }
}

void ota_start(void)
{
    xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL);
}
