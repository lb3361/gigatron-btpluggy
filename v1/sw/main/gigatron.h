#pragma once

#include "esp_err.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

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

#define GIGATRON_QA_GPIO       GPIO_NUM_7
#define GIGATRON_QB_GPIO       GPIO_NUM_8
#define GIGATRON_QC_GPIO       GPIO_NUM_26
#define GIGATRON_QD_GPIO       GPIO_NUM_25
#define GIGATRON_QE_GPIO       GPIO_NUM_19
#define GIGATRON_QF_GPIO       GPIO_NUM_20
#define GIGATRON_QG_GPIO       GPIO_NUM_21
#define GIGATRON_QH_GPIO       GPIO_NUM_22

#define PSEUDO_VBL_GPIO        GPIO_NUM_32

/* Gigatron Shift Register Timing:
 * SERCLK (HSYNC) goes LOW for approximately 4µs per scanline.  When
 * it goes HIGH, a real 74HC595 nearly instantaneously latches the
 * internal shift register on its output pins, and shifts the
 * register.  We plan to interrupt when SERCLK goes LOW and give
 * outselves about 4us to do the job.
 */

typedef struct gigatron_irq_data_s {
  uint8_t *hc595ptr;            /* pointer to byte output to the bus */
  uint8_t  hc595ptrlen;         /* remaining bytes before reverting to hc595state */
  uint8_t  hc595state;          /* state of simulated 74hc595 */
  uint16_t missed;              /* debug: missed hsyncs, should remain zero */
  int videoline;                /* videoline counter */
  int videolines_since_ie;      /* videolines since last /ie pulse */
  int framecount;               /* incremented each frame */
  uint32_t *bytemap;            /* table for setting bus data */
  /* the following are used to initialize hc595ptr on videoline -28 */
  uint8_t inject;               /* overrides serialraw when not 0xff */
  uint8_t *loaderframe;         /* next loader frame to pass */
  int     loaderframelen;       /* loader frame length, 64 or 65 */

} gigatron_irq_data_t;


#ifdef __cplusplus
extern "C" {
#endif

/* data for the nmi handler */
extern gigatron_irq_data_t irq;

/* Initialize Gigatron interface hardware and internal event queue
 * Configures GPIO pins but does not enable interrupts yet.
 *
 * Returns:
 *   - ESP_OK on success
 *   - Error code on failure
 */
esp_err_t gigatron_init(void);


/* The input event queue,
 * potentially useful to time inout delivery.
 */
extern QueueHandle_t input_event_queue;


/* Post input byte into the Gigatron interface.
 * - byte: which value for serialRaw
 * - duration: for how many extra frames
 * - frame: additional data for a loader frame, or null
 * - abs(framelen): length of the additional data.
 * - ticks: how long to wait for the queue.
 * Returns ESP_OK on success or ESP_ERR_TIMEMOUT.
 * Caller retains ownership of mallocated pointer `frame`
 * until this function returns ESP_OK.
 * Negaive framelen indicate that the last frame byte is a checksum.
 * The checksum must be computed with a zero initial value
 * and the queuing code will fix it depending on the presence
 * continued frames.
 */
esp_err_t gigatron_post_input(uint8_t byte, uint8_t duration,
                              uint8_t *frame, int framelen,
                              TickType_t ticks_to_wait);

  
#ifdef __cplusplus
}
#endif
