#pragma once

#include "esp_err.h"
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Set callback for receiving bytes from Gigatron.
 * Callback is called with byte (0-255) or -1 if no valid pattern received
 * for more than 2 frames. RMT is initialized when a valid cb is provided.
 * Returns:
 *   - ESP_OK on success
 *   - Error code on failure
 */

typedef void (*pluggy_rx_callback_t)(int byte);
esp_err_t pluggy_init(pluggy_rx_callback_t cb);

#ifdef __cplusplus
}
#endif
