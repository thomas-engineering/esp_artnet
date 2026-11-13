#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"

#define LED_GPIO        2    /* plain on/off LED */
#define WS2812_GPIO     48   /* addressable RGB LED */
#define TASK_PERIOD_MS  100
#define ARTNET_TIMEOUT_MS 3000

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static volatile led_state_t  s_state        = LED_PROVISIONING;
static volatile TickType_t   s_last_artnet  = 0;
static volatile bool         s_artnet_seen  = false;
static volatile led_mode_t   s_mode         = LED_MODE_NORMAL;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

/* ── Background task ─────────────────────────────────────────────────────── */

static void led_task(void *arg)
{
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
        tick++;

        led_state_t state = s_state;

        if (s_mode == LED_MODE_MUTE) {
            gpio_set_level(LED_GPIO, 0);
            set_rgb(0, 0, 0);
            continue;
        }
        if (s_mode == LED_MODE_LOCATE) {
            /* White, 5 Hz blink */
            bool on = (tick % 2) == 0;
            gpio_set_level(LED_GPIO, on);
            if (on) set_rgb(40, 40, 40); else set_rgb(0, 0, 0);
            continue;
        }

        switch (state) {

        case LED_PROVISIONING:
            /* Blue, 1 Hz blink (500 ms on / 500 ms off) */
            if (tick % 10 < 5) {
                gpio_set_level(LED_GPIO, 1);
                set_rgb(0, 0, 28);
            } else {
                gpio_set_level(LED_GPIO, 0);
                set_rgb(0, 0, 0);
            }
            break;

        case LED_CONNECTING:
            /* Yellow, 5 Hz blink (100 ms on / 100 ms off) */
            if (tick % 2 < 1) {
                gpio_set_level(LED_GPIO, 1);
                set_rgb(32, 24, 0);
            } else {
                gpio_set_level(LED_GPIO, 0);
                set_rgb(0, 0, 0);
            }
            break;

        case LED_CONNECTED:
            gpio_set_level(LED_GPIO, 0);
            {
                uint32_t since_ms = s_artnet_seen
                    ? (xTaskGetTickCount() - s_last_artnet) * portTICK_PERIOD_MS
                    : UINT32_MAX;
                if (since_ms < ARTNET_TIMEOUT_MS) {
                    set_rgb(0, 28, 0);   /* green — Art-Net active */
                } else {
                    set_rgb(28, 8, 0);   /* amber — no Art-Net data */
                }
            }
            break;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void led_init(void)
{
    /* Plain LED on GPIO2 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(LED_GPIO, 0);

    /* WS2812 on GPIO48 via RMT */
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = WS2812_GPIO,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return;
    }
    led_strip_clear(s_strip);

    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "LED init OK (GPIO2 + WS2812 on GPIO48)");
}

void led_set_state(led_state_t state)
{
    s_state = state;
}

void led_set_mode(led_mode_t mode)
{
    s_mode = mode;
}

led_mode_t led_get_mode(void)
{
    return s_mode;
}

void led_artnet_pulse(void)
{
    s_last_artnet = xTaskGetTickCount();
    s_artnet_seen = true;
}
