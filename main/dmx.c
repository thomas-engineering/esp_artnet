#include "dmx.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_sig_map.h"
#include "esp_log.h"

#define DMX_UART_NUM    UART_NUM_0
#define DMX_BAUD        250000
#define DMX_TX_BUF      1024
#define DMX_RX_BUF      256

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

    ESP_LOGI(TAG, "DMX init: UART0 TX=GPIO%d RX=GPIO%d DE=GPIO%d @ %d baud",
             DMX_TX_GPIO, DMX_RX_GPIO, DMX_DE_GPIO, DMX_BAUD);
}

void dmx_send(const uint8_t *data, uint16_t length)
{
    /* Wait for previous frame to finish transmitting */
    uart_wait_tx_done(DMX_UART_NUM, pdMS_TO_TICKS(50));

    /*
     * Generate BREAK by inverting the idle TX line (≥88 µs low), then MAB
     * (Mark After Break, ≥8 µs high). The pin routing is not touched:
     * re-calling uart_set_pin() per frame fails with "GPIO not usable".
     */
    uart_set_line_inverse(DMX_UART_NUM, UART_SIGNAL_TXD_INV);
    esp_rom_delay_us(100);
    uart_set_line_inverse(DMX_UART_NUM, UART_SIGNAL_INV_DISABLE);
    esp_rom_delay_us(12);

    /* Start code 0x00 (dimmer) + up to 512 channel bytes */
    const uint8_t start_code = 0x00;
    uart_write_bytes(DMX_UART_NUM, &start_code, 1);

    uint16_t len = length > DMX_CHANNELS ? DMX_CHANNELS : length;
    uart_write_bytes(DMX_UART_NUM, data, len);
}
