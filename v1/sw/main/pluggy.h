#pragma once

#include "esp_err.h"
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the Pluggy: mount the program stores and start the Gigatron
 * receive task. The receive callback is internal to pluggy.c.
 * Returns:
 *   - ESP_OK on success
 *   - Error code on failure
 */
esp_err_t pluggy_init(void);

/* Handle a Ctrl+Fn chord pressed on the keyboard (fn = 1..12 for F1..F12).
 * Called from the keyboard layer with the bda of the keyboard that sent it.
 * Ctrl+F2 replays the recorded program. */
void pluggy_handle_ctrl_fn(int fn, const uint8_t *bda);

#ifdef __cplusplus
}
#endif
