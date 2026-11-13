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

/* Indicator mode, set remotely via ArtAddress (AcLedNormal / AcLedMute / AcLedLocate) */
typedef enum {
    LED_MODE_NORMAL,   /* status colours as usual                  */
    LED_MODE_MUTE,     /* all status LEDs off                      */
    LED_MODE_LOCATE,   /* fast white blink so the node can be found */
} led_mode_t;

void       led_set_mode(led_mode_t mode);
led_mode_t led_get_mode(void);

/* Call on every received Art-Net packet to keep the green indicator alive */
void led_artnet_pulse(void);
