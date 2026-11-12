#include "dmx_sniff.h"

#ifdef DMX_DEBUG_SNIFF

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "dmx.h"

#define SNIFF_UART      UART_NUM_1
#define BREAK_MIN_US    60      /* a low period at least this long is a BREAK */

static const char *TAG = "dmx_sniff";

typedef struct {
    uint32_t breaks;
    uint32_t brk_min, brk_max;      /* BREAK length (us)                  */
    uint32_t mab_min, mab_max;      /* MARK AFTER BREAK (us)              */
    uint32_t per_min, per_max;      /* break-to-break period (us)         */
    uint32_t dat_min, dat_max;      /* first start bit .. last edge (us)  */
} edge_stats_t;

static volatile edge_stats_t s_es;
static volatile bool     s_es_reset = true;

/* ISR state */
static int64_t s_t_fall, s_t_prev_edge, s_t_edge_before_fall;
static int64_t s_t_break_fall, s_t_break_end, s_t_first_fall;
static bool    s_wait_mab;

static inline __attribute__((always_inline)) void upd(volatile uint32_t *mn, volatile uint32_t *mx, uint32_t v, bool first)
{
    if (first || v < *mn) *mn = v;
    if (first || v > *mx) *mx = v;
}

static void IRAM_ATTR edge_isr(void *arg)
{
    int64_t t = esp_timer_get_time();
    int lvl = gpio_ll_get_level(&GPIO, DMX_RX_GPIO);
    if (!lvl) {                                 /* falling edge */
        s_t_edge_before_fall = s_t_prev_edge;
        s_t_fall = t;
        if (s_wait_mab) {
            upd(&s_es.mab_min, &s_es.mab_max, (uint32_t)(t - s_t_break_end), s_es.breaks == 1);
            s_t_first_fall = t;
            s_wait_mab = false;
        }
    } else {                                    /* rising edge */
        uint32_t low = (uint32_t)(t - s_t_fall);
        if (low >= BREAK_MIN_US) {
            bool first = s_es_reset;
            if (s_t_break_fall) {
                upd(&s_es.per_min, &s_es.per_max, (uint32_t)(s_t_fall - s_t_break_fall), first);
                if (s_t_first_fall > s_t_break_fall)
                    upd(&s_es.dat_min, &s_es.dat_max,
                        (uint32_t)(s_t_edge_before_fall - s_t_first_fall), first);
            }
            upd(&s_es.brk_min, &s_es.brk_max, low, first);
            s_es_reset = false;
            s_es.breaks++;
            s_t_break_fall = s_t_fall;
            s_t_break_end = t;
            s_wait_mab = true;
        }
    }
    s_t_prev_edge = t;
}

/* ---- slot decoding via UART ---- */
static QueueHandle_t s_q;
static uint8_t  s_rx[600];
static uint32_t s_rx_n;
static uint8_t  s_last[600];
static uint32_t s_last_n;
static uint8_t  s_max_val[8];
static uint32_t s_frames, s_start_nz;

static void finish_frame(void)
{
    if (s_rx_n) {
        s_frames++;
        if (s_rx[0] != 0x00) s_start_nz++;
        memcpy(s_last, s_rx, s_rx_n < sizeof s_last ? s_rx_n : sizeof s_last);
        s_last_n = s_rx_n;
        for (int i = 1; i < 8 && i < (int)s_rx_n; i++)
            if (s_rx[i] > s_max_val[i]) s_max_val[i] = s_rx[i];
    }
    s_rx_n = 0;
}

static void rx_task(void *arg)
{
    uart_event_t ev;
    uint8_t tmp[128];
    for (;;) {
        if (!xQueueReceive(s_q, &ev, portMAX_DELAY)) continue;
        if (ev.type == UART_BREAK) {
            finish_frame();
        } else if (ev.type == UART_DATA) {
            size_t left = ev.size;
            while (left) {
                int n = uart_read_bytes(SNIFF_UART, tmp, left < sizeof tmp ? left : sizeof tmp, 0);
                if (n <= 0) break;
                for (int i = 0; i < n; i++, s_rx_n++)
                    if (s_rx_n < sizeof s_rx) s_rx[s_rx_n] = tmp[i];
                left -= n;
            }
        } else if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            uart_flush_input(SNIFF_UART);
            xQueueReset(s_q);
        }
    }
}

static void report_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        edge_stats_t e = *(const edge_stats_t *)&s_es;
        s_es_reset = true;
        s_es.breaks = 0;
        ESP_LOGI(TAG, "idle level=%d | breaks/2s=%u BREAK %u..%u us | MAB %u..%u us | period %u..%u us | data %u..%u us",
                 gpio_get_level(DMX_RX_GPIO), (unsigned)e.breaks,
                 (unsigned)e.brk_min, (unsigned)e.brk_max,
                 (unsigned)e.mab_min, (unsigned)e.mab_max,
                 (unsigned)e.per_min, (unsigned)e.per_max,
                 (unsigned)e.dat_min, (unsigned)e.dat_max);
        ESP_LOGI(TAG, "frames=%u startcode!=0:%u last len=%u: %02X | %02X %02X %02X %02X %02X %02X | max ch1-6: %u %u %u %u %u %u",
                 (unsigned)s_frames, (unsigned)s_start_nz, (unsigned)s_last_n, s_last[0],
                 s_last[1], s_last[2], s_last[3], s_last[4], s_last[5], s_last[6],
                 s_max_val[1], s_max_val[2], s_max_val[3], s_max_val[4], s_max_val[5], s_max_val[6]);
        memset(s_max_val, 0, sizeof s_max_val);
    }
}

void dmx_sniff_start(void)
{
    gpio_set_level(DMX_DE_GPIO, 0);             /* DE=0 RE=0: receive mode */

    uart_config_t cfg = {
        .baud_rate = 250000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_delete(SNIFF_UART);             /* dmx_init() installed a TX-only setup */
    ESP_ERROR_CHECK(uart_driver_install(SNIFF_UART, 4096, 0, 64, &s_q, 0));
    ESP_ERROR_CHECK(uart_param_config(SNIFF_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(SNIFF_UART, UART_PIN_NO_CHANGE, DMX_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Flush bytes right away so a frame tail is not delayed past the next BREAK */
    uart_set_rx_full_threshold(SNIFF_UART, 1);

    ESP_ERROR_CHECK(gpio_set_intr_type(DMX_RX_GPIO, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(DMX_RX_GPIO, edge_isr, NULL));

    xTaskCreate(rx_task,     "sn_rx",  4096, NULL, 7, NULL);
    xTaskCreate(report_task, "sn_rep", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "sniffer active: DE/RE=0, listening on GPIO%d", DMX_RX_GPIO);
}

#endif /* DMX_DEBUG_SNIFF */
