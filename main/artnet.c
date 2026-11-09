#include "artnet.h"
#include "dmx.h"
#include "led.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "artnet";

/* Art-Net ArtDmx packet layout (all multi-byte fields little-endian except ProtVer) */
typedef struct __attribute__((packed)) {
    uint8_t  id[8];         /* "Art-Net\0" */
    uint16_t opcode;        /* 0x5000 = ArtDmx (LE) */
    uint16_t prot_ver;      /* 14 (BE) */
    uint8_t  sequence;
    uint8_t  physical;
    uint16_t universe;      /* 15-bit port-address (LE) */
    uint16_t length;        /* channel count, big-endian */
    uint8_t  data[DMX_CHANNELS];
} artnet_dmx_t;

#define ARTNET_ID       "Art-Net\0"
#define ARTNET_OP_DMX   0x5000u

static uint8_t           s_buf[DMX_CHANNELS];
static SemaphoreHandle_t s_mutex;
static uint16_t          s_universe;
static volatile uint32_t s_pkt_count = 0;

static void artnet_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(ARTNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening for Art-Net on UDP port %d, universe %d",
             ARTNET_PORT, s_universe);

    static artnet_dmx_t pkt;
    while (1) {
        int len = recv(sock, &pkt, sizeof(pkt), 0);
        if (len < 18) continue;
        if (memcmp(pkt.id, ARTNET_ID, 8) != 0) continue;
        if (pkt.opcode != ARTNET_OP_DMX) continue;
        if (pkt.universe != s_universe) continue; /* LE on LE machine */

        uint16_t data_len = ntohs(pkt.length);
        if (data_len > DMX_CHANNELS) data_len = DMX_CHANNELS;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memcpy(s_buf, pkt.data, data_len);
        xSemaphoreGive(s_mutex);
        led_artnet_pulse();
        s_pkt_count++;
    }
}

void artnet_start(uint16_t universe)
{
    s_universe = universe;
    s_mutex    = xSemaphoreCreateMutex();
    memset(s_buf, 0, DMX_CHANNELS);
    xTaskCreate(artnet_task, "artnet", 4096, NULL, 5, NULL);
}

uint32_t artnet_get_pkt_count(void) { return s_pkt_count; }

void artnet_set_universe(uint16_t universe) { s_universe = universe; }

void artnet_copy_dmx(uint8_t *dst, uint16_t max_len)
{
    uint16_t n = max_len < DMX_CHANNELS ? max_len : DMX_CHANNELS;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(dst, s_buf, n);
    xSemaphoreGive(s_mutex);
}
