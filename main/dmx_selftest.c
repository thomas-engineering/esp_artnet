#include "dmx_selftest.h"

#ifdef DMX_DEBUG_READBACK

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_timer.h"
#include "esp_log.h"

#define RB_DATA_UART    UART_NUM_2   /* 250 kbaud 8N2: decodes the frame (UART0 = console, UART1 = DMX) */
#define RB_DATA_BAUD    250000
#define DMX_MAX_SLOTS   512
#define RB_BUF          (DMX_MAX_SLOTS + 8)

static const char *TAG = "dmx_rb";

typedef struct {
    uint32_t breaks;        /* breaks seen (low >= 44 us)                   */
    uint32_t ok;            /* frames that match what dmx_send() sent       */
    uint32_t bad_len;
    uint32_t bad_start;
    uint32_t bad_data;
    uint32_t phantom;       /* frames with one extra 0x00 from the break    */
    uint32_t frame_err;
    uint32_t overflow;
    uint32_t period_min_us;
    uint32_t period_max_us;
} rb_stats_t;

static rb_stats_t s_stats;
static volatile bool s_stats_reset_period = true;

static uint8_t  s_rx[RB_BUF];
static uint32_t s_rx_n;                 /* bytes seen since last break (may exceed RB_BUF) */
static uint8_t  s_exp_cur[DMX_MAX_SLOTS],  s_exp_prev[DMX_MAX_SLOTS];
static uint16_t s_len_cur, s_len_prev;
static int64_t  s_last_break_us;

static uint32_t s_last_n;               /* details of the last frame, for the report */
static uint8_t  s_last_head[32];
static char     s_last_err[96];

static QueueHandle_t s_q_data;

/* ---- frame comparison ---- */
static void finish_frame(void)
{
    if (s_len_prev == 0) {
        s_rx_n = 0;
        return;
    }
    uint32_t total = 1u + s_len_prev;
    uint32_t off;
    s_last_n = s_rx_n;
    memcpy(s_last_head, s_rx, sizeof s_last_head);
    s_last_err[0] = '\0';

    if (s_rx_n == total) {
        off = 0;
    } else if (s_rx_n == total + 1 && s_rx[0] == 0x00) {
        off = 1;
        s_stats.phantom++;
    } else {
        s_stats.bad_len++;
        snprintf(s_last_err, sizeof s_last_err, "length %u, expected %u", (unsigned)s_rx_n, (unsigned)total);
        s_rx_n = 0;
        return;
    }

    if (s_rx[off] != 0x00) {
        s_stats.bad_start++;
        snprintf(s_last_err, sizeof s_last_err, "start code 0x%02X, expected 0x00", s_rx[off]);
    } else {
        for (uint32_t i = 0; i < s_len_prev; i++) {
            if (s_rx[off + 1 + i] != s_exp_prev[i]) {
                s_stats.bad_data++;
                snprintf(s_last_err, sizeof s_last_err, "slot %u: got 0x%02X, expected 0x%02X",
                         (unsigned)(i + 1), s_rx[off + 1 + i], s_exp_prev[i]);
                s_rx_n = 0;
                return;
            }
        }
        s_stats.ok++;
    }
    s_rx_n = 0;
}

static void data_task(void *arg)
{
    uart_event_t ev;
    uint8_t tmp[128];

    for (;;) {
        if (!xQueueReceive(s_q_data, &ev, portMAX_DELAY)) {
            continue;
        }
        switch (ev.type) {
        case UART_BREAK: {
            finish_frame();
            int64_t now = esp_timer_get_time();
            if (s_last_break_us) {
                uint32_t p = (uint32_t)(now - s_last_break_us);
                if (s_stats_reset_period) {
                    s_stats.period_min_us = s_stats.period_max_us = p;
                    s_stats_reset_period = false;
                } else {
                    if (p < s_stats.period_min_us) s_stats.period_min_us = p;
                    if (p > s_stats.period_max_us) s_stats.period_max_us = p;
                }
            }
            s_last_break_us = now;
            s_stats.breaks++;
            break;
        }
        case UART_DATA: {
            size_t left = ev.size;
            while (left) {
                int n = uart_read_bytes(RB_DATA_UART, tmp, left < sizeof tmp ? left : sizeof tmp, 0);
                if (n <= 0) break;
                for (int i = 0; i < n; i++) {
                    if (s_rx_n < RB_BUF) s_rx[s_rx_n] = tmp[i];
                    s_rx_n++;
                }
                left -= n;
            }
            break;
        }
        case UART_FRAME_ERR:
            s_stats.frame_err++;
            break;
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            s_stats.overflow++;
            uart_flush_input(RB_DATA_UART);
            xQueueReset(s_q_data);
            break;
        default:
            break;
        }
    }
}

static void report_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        const rb_stats_t s = s_stats;
        ESP_LOGI(TAG, "breaks>=44us=%u | frames ok=%u badlen=%u badstart=%u baddata=%u | "
                      "ferr=%u ovf=%u phantom0=%u | period %u..%u us",
                 (unsigned)s.breaks, (unsigned)s.ok, (unsigned)s.bad_len,
                 (unsigned)s.bad_start, (unsigned)s.bad_data, (unsigned)s.frame_err,
                 (unsigned)s.overflow, (unsigned)s.phantom,
                 (unsigned)s.period_min_us, (unsigned)s.period_max_us);
        char hex[32 * 3 + 1];
        for (int i = 0; i < 32; i++) sprintf(&hex[i * 3], "%02X ", s_last_head[i]);
        ESP_LOGI(TAG, "last frame: %u bytes, head=%s%s%s", (unsigned)s_last_n, hex,
                 s_last_err[0] ? " ERROR: " : "", s_last_err);
    }
}

static void setup_rx(uart_port_t port, int baud, uint32_t sig, int tap, QueueHandle_t *q)
{
    uart_config_t cfg = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_2,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(port, 2048, 0, 64, q, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    uart_set_rx_full_threshold(port, 1); /* deliver every byte at once: no frame tail left in the FIFO at the next BREAK */
    /* Feed the pad level into this UART's RX through the GPIO matrix. The pad
     * keeps its UART1 TX output function; we only enable its input path. */
    ESP_ERROR_CHECK(gpio_input_enable(tap));
    esp_rom_gpio_connect_in_signal(tap, sig, false);
}

void dmx_selftest_expect(const uint8_t *data, uint16_t len)
{
    if (len > DMX_MAX_SLOTS) len = DMX_MAX_SLOTS;
    memcpy(s_exp_prev, s_exp_cur, sizeof s_exp_prev);
    s_len_prev = s_len_cur;
    memcpy(s_exp_cur, data, len);
    s_len_cur = len;
}

void dmx_selftest_start(int tap_gpio)
{
    setup_rx(RB_DATA_UART, RB_DATA_BAUD, U2RXD_IN_IDX, tap_gpio, &s_q_data);

    xTaskCreate(data_task,   "rb_data",   4096, NULL, 7, NULL);
    xTaskCreate(report_task, "rb_report", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "read-back test active on GPIO%d", tap_gpio);
}

#endif /* DMX_DEBUG_READBACK */
