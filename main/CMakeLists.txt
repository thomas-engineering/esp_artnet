idf_component_register(
    SRCS
        "main.c"
        "artnet.c"
        "artnet_poll.c"
        "artnet_ip.c"
        "dmx.c"
        "provisioning.c"
        "ota.c"
        "led.c"
        "webserver.c"
    INCLUDE_DIRS "." "debug"
    EMBED_TXTFILES
        "web/status.html"
        "web/portal.html"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_https_ota
        esp_http_client
        esp-tls
        app_update
        esp_driver_uart
        esp_driver_gpio
        esp_rom
        esp_netif
        esp_event
        esp_http_server
        lwip
        mdns
        led_strip
        esp_timer
)

# Optional debug / test features live in main/debug/ and are only compiled
# when requested, e.g.: idf.py -DDMX_DEBUG_REPLAY=1 -DDMX_DEBUG_READBACK=1 build
include("${CMAKE_CURRENT_LIST_DIR}/debug/debug.cmake")
