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

/* Device kind, as saved in NVS by the hid layer. */
#define GAP_KIND_UNKNOWN    0
#define GAP_KIND_KEYBOARD   1
#define GAP_KIND_GAMEPAD    2

/* Info about a bonded device, for status display. */
typedef struct {
    uint8_t bda[6];
    bool    ble;        /* false = BT Classic */
    bool    connected;
    uint8_t kind;       /* GAP_KIND_* */
    char    name[64];   /* empty when never saved */
} gap_device_info_t;

/* Fill list with bonded devices (BT Classic first, then BLE).
 * Returns the number of entries written, at most max. */
int gap_list_devices(gap_device_info_t *list, int max);

/* Copy the saved name of a device into name (size bytes),
 * or its bda when no name was saved. */
void gap_device_name(const uint8_t *bda, char *name, int size);

/* Remove all bonded devices. */
esp_err_t gap_clear_all_bonds(void);
