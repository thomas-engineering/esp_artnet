#pragma once

typedef enum {
    LED_PROVISIONING,  /* SoftAP active        — blue slow blink  */
    LED_CONNECTING,    /* STA connect attempt  — yellow fast blink */
    LED_CONNECTED,     /* WiFi connected       — green / amber     */
} led_state_t;

/* Initialise GPIO2 and WS2812 on GPIO48, start background task */
void led_init(void);

/* Switch to a new named state */
void led_set_state(led_state_t state);

/* Call on every received Art-Net packet to keep the green indicator alive */
void led_artnet_pulse(void);
