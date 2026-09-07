#pragma once

#include "hid_parser.h"
#include <stdint.h>
#include <stdbool.h>

struct esp_hidh_dev_s;

/* ── event types ──────────────────────────────────────────────────── */

typedef enum {
    KB_KEY_DOWN,
    KB_KEY_UP,
} kb_event_type_t;

typedef struct {
    kb_event_type_t type;
    uint8_t scancode;      /* HID usage-page 0x07 usage ID */
    uint8_t modifiers;     /* current modifier bitfield */
    uint8_t giga_buttons;
    uint8_t giga_key;
} kb_event_t;

/* Modifier bit definitions (same as HID boot protocol byte) */
#define KB_MOD_LCTRL   (1 << 0)
#define KB_MOD_LSHIFT  (1 << 1)
#define KB_MOD_LALT    (1 << 2)
#define KB_MOD_LGUI    (1 << 3)
#define KB_MOD_RCTRL   (1 << 4)
#define KB_MOD_RSHIFT  (1 << 5)
#define KB_MOD_RALT    (1 << 6)
#define KB_MOD_RGUI    (1 << 7)

/* Keyboard callback after gigatron-specific processing.
   - The callback is invoked whenever the set of depressed button keys
     changes.  Field ev->giga_buttons represents this set in negative
     logic.  Value 0xff means that no button key is depressed.
   - The callback is also invoked whenever a key press indicates a
     valid ascii code with no button key depressed. The ascii code is
     provided in ev->giga_key.  When the last reported key is
     released, the callback is called with both ev->giga_buttons and
     ev->giga_key equal to 0xff.
   - When the user pressed ctrl+alt+del, the callback is invoked with
     both ev->giga_buttons and ev->giga_key equal to 0xef.
   All other keyboard events are either discarded or processed
   internally.  For instance ctrl+alt+Fn selects keymap n-1. */
typedef void (*kb_event_cb_t)(const kb_event_t *ev, void *user_data);

/* ── opaque handle ────────────────────────────────────────────────── */

typedef struct keyboard_s keyboard_t;

/* ── API ──────────────────────────────────────────────────────────── */

/* Create a keyboard decoder from a parsed field map.
 * Returns NULL if the map contains no keyboard report. */
keyboard_t *keyboard_create(const hid_field_map_t *map,
                            struct esp_hidh_dev_s *dev,
                            kb_event_cb_t cb, void *user_data);

void keyboard_destroy(keyboard_t *kb);

/* Feed a raw HID INPUT report.  Generates KEY_DOWN / KEY_UP events
 * via the callback for every key state change. */
void keyboard_process_report(keyboard_t *kb,
                             struct esp_hidh_dev_s *dev,
                             uint8_t report_id,
                             const uint8_t *data, uint16_t len);

/* Query the current modifier byte. */
uint8_t keyboard_get_modifiers(const keyboard_t *kb);

/* Query the currently pressed key scan-codes.
 * Returns a pointer to an internal array; *count receives the
 * number of valid entries.  Valid until the next process_report. */
const uint8_t *keyboard_get_pressed_keys(const keyboard_t *kb, int *count);
