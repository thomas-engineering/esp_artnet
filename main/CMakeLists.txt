idf_component_register(
    SRCS
        "main.c"
        "artnet.c"
        "dmx.c"
        "dmx_selftest.c"
        "provisioning.c"
        "ota.c"
        "led.c"
        "webserver.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_https_ota
        esp_http_client
        esp-tls
        app_update
        esp_driver_uart
        esp_driver_gpio
        esp_driver_usb_serial_jtag
        esp_rom
        esp_netif
        esp_event
        esp_http_server
        lwip
        mdns
        led_strip
        esp_timer
)

# Optional debug features: idf.py -DDMX_DEBUG_TEST_PATTERN=1 -DDMX_DEBUG_READBACK=1 build
if(DMX_DEBUG_TEST_PATTERN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DMX_DEBUG_TEST_PATTERN)
endif()
if(DMX_DEBUG_READBACK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DMX_DEBUG_READBACK)
endif()
