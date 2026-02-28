#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GAP_EVT_PAIRING_START,
    GAP_EVT_PAIRING_END,
    GAP_EVT_DEVICE_CONNECTED,
    GAP_EVT_DEVICE_DISCONNECTED,
} gap_event_t;

typedef void (*gap_event_cb_t)(gap_event_t event, void *param);

/* Initialize GAP layer for BT Classic + BLE HID host.
 * Must be called after Bluedroid is enabled. */
esp_err_t gap_init(gap_event_cb_t callback);

/* Enter pairing mode for timeout_seconds. Scans for keyboards/gamepads,
 * connects, pairs, bonds. */
esp_err_t gap_start_pairing(uint32_t timeout_seconds);

/* Exit pairing mode early. */
esp_err_t gap_stop_pairing(void);

/* True if currently in pairing mode. */
bool gap_is_pairing_active(void);

/* Number of bonded devices (BT Classic + BLE combined). */
int gap_get_bonded_count(void);

/* Remove all bonded devices. */
esp_err_t gap_clear_all_bonds(void);
