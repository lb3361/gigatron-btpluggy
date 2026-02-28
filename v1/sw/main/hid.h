#pragma once

#include "esp_err.h"
#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "keyboard.h"
#include "gamepad.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    esp_hidh_dev_t *dev;
    esp_hid_transport_t transport;
    esp_hid_usage_t usage;
    uint8_t  map_index;
    uint16_t report_id;
    const uint8_t *data;
    uint16_t length;
} hid_report_t;

typedef void (*hid_report_cb_t)(const hid_report_t *report);
typedef void (*hid_connection_cb_t)(esp_hidh_dev_t *dev, bool connected);

/* Initialize HID host layer.
 * Must be called after Bluedroid is enabled and GAP is initialized. */
esp_err_t hid_init(hid_report_cb_t report_cb, hid_connection_cb_t conn_cb);

/* Number of currently connected HID devices. */
int hid_get_connected_count(void);

/* Check if a device with the given BDA is currently connected. */
bool hid_is_device_connected(const uint8_t *bda);

/* Set keyboard event callback (called for every key press/release). */
void hid_set_keyboard_cb(kb_event_cb_t cb, void *user_data);

/* Set gamepad event callback (called on every state change). */
void hid_set_gamepad_cb(gp_event_cb_t cb, void *user_data);
