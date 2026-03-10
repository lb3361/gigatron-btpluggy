#pragma once

#include "esp_err.h"
#include <stdint.h>

/* Initialize LEDs:
 *   GPIO13 = red LED (power indicator, always on)
 *   GPIO2  = NeoPixel power (set HIGH)
 *   GPIO0  = NeoPixel data (WS2812, driven via RMT) */
esp_err_t led_init(void);

/* Set persistent state (flashing with period, period_ms=0 means solid) */
void led_set_persistent(uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms);

/* Set transient state (auto-expires to persistent state) */
void led_set_transient(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms);

