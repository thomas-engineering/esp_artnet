#include "dmx.h"
#include "dmx_selftest.h"
#include "dmx_sniff.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/uart_ll.h"
#include "soc/gpio_sig_map.h"
#include "esp_log.h"

#define DMX_UART_NUM    UART_NUM_1   /* UART0 stays the console */
#define DMX_BAUD        250000
#define DMX_TX_BUF      1024
#define DMX_RX_BUF      256

/* Same timing as the working Arduino (Conceptinetics) master: a 0x00 byte at
 * 49950 baud 8N2 = 180 us BREAK + 40 us MARK AFTER BREAK. */
#define DMX_BREAK_US    180
#define DMX_MAB_US      40

static const char *TAG = "dmx";

void dmx_init(void)
{
    gpio_config_t de_cfg = {
        .pin_bit_mask = (1ULL << DMX_DE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&de_cfg);
    gpio_set_level(DMX_DE_GPIO, 1); /* DE=1 RE=1: transmit mode */

    uart_config_t uart_cfg = {
        .baud_rate  = DMX_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_2,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(DMX_UART_NUM, DMX_RX_BUF, DMX_TX_BUF, 0, NULL, 0);
    uart_param_config(DMX_UART_NUM, &uart_cfg);
    uart_set_pin(DMX_UART_NUM, DMX_TX_GPIO, DMX_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "DMX init: UART1 TX=GPIO%d RX=GPIO%d DE=GPIO%d @ %d baud",
             DMX_TX_GPIO, DMX_RX_GPIO, DMX_DE_GPIO, DMX_BAUD);

    dmx_selftest_start(DMX_TX_GPIO); /* no-op unless -DDMX_DEBUG_READBACK */
#ifdef DMX_DEBUG_SNIFF
    dmx_sniff_start();               /* listen only, never transmit */
#endif
}

void dmx_send(const uint8_t *data, uint16_t length)
{
#ifdef DMX_DEBUG_SNIFF
    (void)data; (void)length;
    return; /* sniffer mode: the bus belongs to the other master */
#endif
    /* Wait for previous frame to finish transmitting */
    uart_wait_tx_done(DMX_UART_NUM, pdMS_TO_TICKS(50));

    dmx_selftest_expect(data, length > DMX_CHANNELS ? DMX_CHANNELS : length);

    /*
     * Generate BREAK by inverting the idle TX line (180 us low), then MAB
     * (Mark After Break, 40 us high) - the same timing as the Arduino
     * Conceptinetics master. The pin routing is not touched: re-calling
     * uart_set_pin() per frame fails with "GPIO not usable".
     *
     * The start code is written straight into the TX FIFO right after the
     * MAB: going through uart_write_bytes() added ~90 us of ISR latency. The
     * whole sequence runs in a critical section so it cannot be stretched by
     * preemption.
     */
    const uint8_t start_code = 0x00; /* 0x00 = dimmer data */
    static portMUX_TYPE brk_lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&brk_lock);
    uart_set_line_inverse(DMX_UART_NUM, UART_SIGNAL_TXD_INV);
    esp_rom_delay_us(DMX_BREAK_US);
    uart_set_line_inverse(DMX_UART_NUM, UART_SIGNAL_INV_DISABLE);
    esp_rom_delay_us(DMX_MAB_US);
    uart_ll_write_txfifo(UART_LL_GET_HW(DMX_UART_NUM), &start_code, 1);
    portEXIT_CRITICAL(&brk_lock);

    /* Up to 512 channel bytes follow the start code */
    uint16_t len = length > DMX_CHANNELS ? DMX_CHANNELS : length;
    uart_write_bytes(DMX_UART_NUM, data, len);
}
