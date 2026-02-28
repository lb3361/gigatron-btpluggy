#pragma once

#include "esp_err.h"
#include <stdint.h>

/* Initialize LEDs:
 *   GPIO13 = red LED (power indicator, always on)
 *   GPIO2  = NeoPixel power (set HIGH)
 *   GPIO0  = NeoPixel data (WS2812, driven via RMT) */
esp_err_t led_init(void);

/* Set NeoPixel to an arbitrary RGB color. */
void led_set_neopixel(uint8_t r, uint8_t g, uint8_t b);

/* Turn NeoPixel off. */
void led_neopixel_off(void);

/* Start flashing blue at ~2 Hz. */
void led_start_flashing_blue(void);

/* Stop flashing. Does NOT change the current color. */
void led_stop_flashing(void);

/* Set solid blue and stop any flashing. */
void led_set_solid_blue(void);

