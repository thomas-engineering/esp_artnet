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
#include "driver/usb_serial_jtag.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_timer.h"
#include "esp_log.h"

#define RB_DATA_UART    UART_NUM_1   /* 250 kbaud 8N2: decodes the frame */
#define RB_BRK_UART     UART_NUM_2   /* 120 kbaud: break event only if low >= 11 bits = 91.7 us */
#define RB_DATA_BAUD    250000
#define RB_BRK_BAUD     120000
#define DMX_MAX_SLOTS   512
#define RB_BUF          (DMX_MAX_SLOTS + 8)

static const char *TAG = "dmx_rb";

typedef struct {
    uint32_t breaks;        /* breaks seen by UART1 (low >= 44 us)          */
    uint32_t long_breaks;   /* breaks seen by UART2 (low >= 92 us)          */
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
static uint8_t  s_last_head[12];
static char     s_last_err[96];

static QueueHandle_t s_q_data, s_q_brk;

/* ---- log redirect: keep log text off the DMX line (UART0) ---- */
static int usb_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) {
        usb_serial_jtag_write_bytes(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1, 0);
    }
    return n;
}

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

static void brk_task(void *arg)
{
    uart_event_t ev;
    for (;;) {
        if (!xQueueReceive(s_q_brk, &ev, portMAX_DELAY)) {
            continue;
        }
        if (ev.type == UART_BREAK) {
            s_stats.long_breaks++;
        } else if (ev.type == UART_DATA || ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            /* data at the wrong baud rate is garbage by design: discard it */
            uart_flush_input(RB_BRK_UART);
            if (ev.type != UART_DATA) xQueueReset(s_q_brk);
        }
    }
}

static void report_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        const rb_stats_t s = s_stats;
        ESP_LOGI(TAG, "breaks>=44us=%u breaks>=92us=%u | frames ok=%u badlen=%u badstart=%u baddata=%u | "
                      "ferr=%u ovf=%u phantom0=%u | period %u..%u us",
                 (unsigned)s.breaks, (unsigned)s.long_breaks, (unsigned)s.ok, (unsigned)s.bad_len,
                 (unsigned)s.bad_start, (unsigned)s.bad_data, (unsigned)s.frame_err,
                 (unsigned)s.overflow, (unsigned)s.phantom,
                 (unsigned)s.period_min_us, (unsigned)s.period_max_us);
        ESP_LOGI(TAG, "last frame: %u bytes, head=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X%s%s",
                 (unsigned)s_last_n,
                 s_last_head[0], s_last_head[1], s_last_head[2], s_last_head[3], s_last_head[4], s_last_head[5],
                 s_last_head[6], s_last_head[7], s_last_head[8], s_last_head[9], s_last_head[10], s_last_head[11],
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
    /* Feed the pad level into this UART's RX through the GPIO matrix. The pad
     * keeps its UART0 TX output function; we only enable its input path. */
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
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_cfg.tx_buffer_size = 4096;
    esp_err_t err = usb_serial_jtag_driver_install(&usb_cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        esp_log_set_vprintf(usb_vprintf);
    }

    setup_rx(RB_DATA_UART, RB_DATA_BAUD, U1RXD_IN_IDX, tap_gpio, &s_q_data);
    setup_rx(RB_BRK_UART,  RB_BRK_BAUD,  U2RXD_IN_IDX, tap_gpio, &s_q_brk);

    xTaskCreate(data_task,   "rb_data",   4096, NULL, 7, NULL);
    xTaskCreate(brk_task,    "rb_brk",    3072, NULL, 5, NULL);
    xTaskCreate(report_task, "rb_report", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "read-back test active on GPIO%d (usb driver: %s)", tap_gpio, esp_err_to_name(err));
}

#endif /* DMX_DEBUG_READBACK */
