#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "driver/gpio.h"
#include "artnet.h"
#include "dmx.h"
#include "debug/dmx_debug.h"
#include "provisioning.h"
#include "ota.h"
#include "led.h"
#include "webserver.h"

/* GPIO0 = BOOT button on Freenove ESP32-S3-WROOM.
   Hold for 3 s while the node is running to reset all settings. (Holding it
   during power-on does not work: GPIO0 low at reset selects download mode.) */
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
        dmx_debug_override_frame(buf, DMX_CHANNELS); /* no-op in production builds */
        dmx_send(buf, DMX_CHANNELS);
        /* No extra pause: dmx_send() waits for the previous frame, giving ~43 Hz
         * back-to-back frames like the Arduino reference master. */
        if (!dmx_debug_frame_pause()) vTaskDelay(1);
    }
}

/* A new address (DHCP renewal, reconnect) is announced to Art-Net controllers */
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    artnet_announce();
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

    /* DMX runs from the start, independent of the network: the bus is driven
       (idle frames) while the node connects or sits in the setup portal.
       Pinned to core 1 so that its short interrupts-off BREAK sequence does
       not delay the WiFi stack on core 0. */
    dmx_init();           /* UART1 on GPIO41/42 + DE on GPIO1 */
    xTaskCreatePinnedToCore(dmx_output_task, "dmx_out", 4096, NULL, 6, NULL, 1);
    xTaskCreate(boot_button_task, "boot_btn", 4096, NULL, 3, NULL);

    provisioning_start(); /* blocks until IP obtained */

    mdns_init();
    mdns_hostname_set("dmx-artnet");
    mdns_instance_name_set("Art-Net DMX Node");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI("main", "mDNS hostname: dmx-artnet.local");

    artnet_start(provisioning_get_universe());
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    webserver_start();
    ota_start();

    /* Rollback is enabled: a freshly updated image stays "pending" until it
       is confirmed, and would be rolled back at the next reset. Reaching this
       point (WiFi connected, all services started) is the self-test. */
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware confirmed");
    }
}
