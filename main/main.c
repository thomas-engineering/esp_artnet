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
#include "esp_rom_sys.h"
#include "esp_timer.h"
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
#ifdef DMX_DEBUG_REPLAY
        /* Replay of the Arduino DMX_Master sketch as captured on the bus:
         * ch1-3 fade 0..31..0 in steps of 1 every 30 ms, all other slots 0,
         * 513 slots per frame, one frame every ~26 ms. */
        static int val, dir = 1;
        static int64_t t_step;
        int64_t now = esp_timer_get_time();
        if (now - t_step >= 30000) {
            t_step = now;
            val += dir;
            if (val > 30) dir = -1;
            if (val < 1)  dir = 1;
        }
        memset(buf, 0, sizeof buf);
        buf[0] = buf[1] = buf[2] = (uint8_t)val;
#endif
#ifdef DMX_DEBUG_TEST_PATTERN
        memset(buf, 255, 8); /* TEMP: channels 1-8 full, bypasses Art-Net */
#endif
        dmx_send(buf, DMX_CHANNELS);
        /* No extra pause: dmx_send() waits for the previous frame, giving ~43 Hz
         * back-to-back frames like the Arduino reference master. */
#ifdef DMX_DEBUG_REPLAY
        esp_rom_delay_us(3300); /* UNO frame period is ~26.1 ms (6 us gap per slot) */
#else
        vTaskDelay(1);
#endif
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

    provisioning_start(); /* blocks until IP obtained */

    mdns_init();
    mdns_hostname_set("dmx-artnet");
    mdns_instance_name_set("ArtNet DMX Node");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI("main", "mDNS hostname: dmx-artnet.local");

    dmx_init();           /* UART1 on GPIO41/42 + DE on GPIO1 */
    artnet_start(provisioning_get_universe());
    webserver_start();
    ota_start();

    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);
    xTaskCreate(dmx_output_task,  "dmx_out",  4096, NULL, 6, NULL);
}
