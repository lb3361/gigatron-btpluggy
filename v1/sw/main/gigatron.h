#pragma once

#include "esp_err.h"
#include <stdint.h>

/*
 * Gigatron Interface Pin Mapping
 * 
 * ESP32 GPIO Pin Assignments:
 * 
 * INPUTS (connected to Gigatron board):
 *   - SERCLK: GPIO36 (HSYNC signal from Gigatron VGA)
 *   - /IE:    GPIO37 (Active LOW - Output Enable for level shifter)
 *   - SERIN:  GPIO38 (Serial data input from 74HC595)
 *
 * OUTPUTS (connected to Gigatron bus via 74LVC245 level shifter):
 *   - Q0: GPIO7  (Data bit 0 - LSB)
 *   - Q1: GPIO8  (Data bit 1)
 *   - Q2: GPIO26 (Data bit 2)
 *   - Q3: GPIO25 (Data bit 3)
 *   - Q4: GPIO19 (Data bit 4)
 *   - Q5: GPIO20 (Data bit 5)
 *   - Q6: GPIO21 (Data bit 6)
 *   - Q7: GPIO22 (Data bit 7 - MSB)
 *
 * Notes:
 * - The level shifter is controlled by /IE pin (GPIO37)
 * - When /IE = LOW, outputs are enabled to Gigatron bus
 * - When /IE = HIGH, outputs are tri-stated (Gigatron drives the bus)
 * - SERCLK is the HSYNC signal which goes LOW for ~4µs every scanline
 * - SERIN receives serial data shifted in during HSYNC LOW period
 */

#define GIGATRON_SERCLK_GPIO   GPIO_NUM_36
#define GIGATRON_IE_GPIO       GPIO_NUM_37
#define GIGATRON_SERIN_GPIO    GPIO_NUM_38

#define GIGATRON_Q0_GPIO       GPIO_NUM_7
#define GIGATRON_Q1_GPIO       GPIO_NUM_8
#define GIGATRON_Q2_GPIO       GPIO_NUM_26
#define GIGATRON_Q3_GPIO       GPIO_NUM_25
#define GIGATRON_Q4_GPIO       GPIO_NUM_19
#define GIGATRON_Q5_GPIO       GPIO_NUM_20
#define GIGATRON_Q6_GPIO       GPIO_NUM_21
#define GIGATRON_Q7_GPIO       GPIO_NUM_22

/* Gigatron Shift Register Timing:
 * SERCLK (HSYNC) goes LOW for approximately 4µs per scanline.  When
 * it goes HIGH, a real 74HC595 nearly instantaneously latches the
 * internal shift register on its output pins, and shifts the
 * register.  We plan to interrupt when SERCLK goes LOW and give
 * outselves about 4us to do the job.
 */

/* Gigatron task configuration */
#define GIGATRON_TASK_PRIORITY  5
#define GIGATRON_TASK_STACK     4096
#define GIGATRON_TASK_CORE      1

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize Gigatron interface hardware
 * Configures GPIO pins but does not enable interrupts yet.
 * 
 * Returns:
 *   - ESP_OK on success
 *   - Error code on failure
 */
esp_err_t gigatron_init(void);

#ifdef __cplusplus
}
#endif
