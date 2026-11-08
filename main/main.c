#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "driver/gpio.h"
#include "artnet.h"
#include "dmx.h"
#include "provisioning.h"
#include "ota.h"
#include "led.h"
#include "webserver.h"

/* GPIO0 = BOOT button on Freenove ESP32-S3-WROOM.
   Hold for 3 s (at any time, not only at power-on) to clear WiFi credentials. */
#define BOOT_GPIO               0
#define FACTORY_RESET_HOLD_MS   3000
#define BOOT_POLL_MS            50

static const char *TAG = "main";

static void check_factory_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOOT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    if (gpio_get_level(BOOT_GPIO) == 0) {
        ESP_LOGW(TAG, "BOOT held — release within %d ms to cancel factory reset",
                 FACTORY_RESET_HOLD_MS);
        vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_HOLD_MS));
        if (gpio_get_level(BOOT_GPIO) == 0) {
            ESP_LOGW(TAG, "Factory reset: clearing WiFi credentials");
            provisioning_clear();
            esp_restart();
        }
    }
}

/* Polls BOOT button at runtime; holds for FACTORY_RESET_HOLD_MS → factory reset */
static void boot_button_task(void *arg)
{
    uint32_t held_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
        if (gpio_get_level(BOOT_GPIO) == 0) {
            held_ms += BOOT_POLL_MS;
            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held — clearing WiFi credentials and restarting");
                provisioning_clear();
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}

/* Sends DMX frames at ~40 Hz using Art-Net channel data */
static void dmx_output_task(void *arg)
{
    uint8_t buf[DMX_CHANNELS];
    while (1) {
        artnet_copy_dmx(buf, DMX_CHANNELS);
        dmx_send(buf, DMX_CHANNELS);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Firmware version: %s", esp_app_get_description()->version);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    led_init();
    check_factory_reset();

    provisioning_start(); /* blocks until IP obtained — UART0 stays as console until here */

    mdns_init();
    mdns_hostname_set("dmx-artnet");
    mdns_instance_name_set("ArtNet DMX Node");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI("main", "mDNS hostname: dmx-artnet.local");

    dmx_init();           /* reconfigures UART0 (GPIO43/44) for DMX after WiFi is up */
    artnet_start(provisioning_get_universe());
    webserver_start();
    ota_start();

    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);
    xTaskCreate(dmx_output_task,  "dmx_out",  4096, NULL, 6, NULL);
}
