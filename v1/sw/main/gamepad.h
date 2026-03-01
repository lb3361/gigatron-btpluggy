#pragma once

#include "hid_parser.h"
#include <stdint.h>
#include <stdbool.h>

struct esp_hidh_dev_s;

/* ── named buttons ────────────────────────────────────────────────── */

typedef enum {
    GP_BTN_A,
    GP_BTN_B,
    GP_BTN_START,
    GP_BTN_SELECT,
    GP_BTN_COUNT
} gp_button_t;

/* ── normalized state ─────────────────────────────────────────────── */

typedef struct {
    bool     buttons[GP_BTN_COUNT];   /* named buttons (after mapping) */
    bool     dpad_up;
    bool     dpad_down;
    bool     dpad_left;
    bool     dpad_right;
    uint16_t raw_buttons;             /* raw bitfield (pre-mapping) */
} gp_state_t;

/* ── button profile ───────────────────────────────────────────────── */

/* Forward declaration for fixup function signature */
typedef struct gamepad_s gamepad_t;

/* Called after descriptor parsing + profile assignment to apply
 * device-specific corrections (e.g., remap buttons based on
 * firmware version, patch dpad sources, etc.). */
typedef void (*gp_fixup_fn)(gamepad_t *gp);

typedef struct {
    const char *name;
    int8_t map[GP_BTN_COUNT];   /* map[GP_BTN_A] = raw button index
                                 *  (0 = Button 1).  -1 = unmapped */
    gp_fixup_fn fixup;          /* NULL = no fixup needed */
} gp_profile_t;

/* Built-in profiles */
extern const gp_profile_t GP_PROFILE_XBOX;
extern const gp_profile_t GP_PROFILE_NINTENDO;
extern const gp_profile_t GP_PROFILE_PLAYSTATION;
extern const gp_profile_t GP_PROFILE_8BITDO;
extern const gp_profile_t GP_PROFILE_GENERIC;

/* ── callback ─────────────────────────────────────────────────────── */

/* Called on every state change (not every report). */
typedef void (*gp_event_cb_t)(const gp_state_t *state, void *user_data);

/* ── API ──────────────────────────────────────────────────────────── */

/* Create a gamepad decoder from a parsed field map.
 * vid/pid are used to auto-select a button profile.
 * Returns NULL if the map contains no gamepad report. */
gamepad_t *gamepad_create(const hid_field_map_t *map,
                          struct esp_hidh_dev_s *dev,
                          gp_event_cb_t cb, void *user_data);

void gamepad_destroy(gamepad_t *gp);

/* Override the auto-detected button profile. */
void gamepad_set_profile(gamepad_t *gp, const gp_profile_t *profile);

/* Feed a raw HID INPUT report. */
void gamepad_process_report(gamepad_t *gp,
                            struct esp_hidh_dev_s *dev,
                            uint8_t report_id,
                            const uint8_t *data, uint16_t len);

/* Query current state (valid until next process_report). */
const gp_state_t *gamepad_get_state(const gamepad_t *gp);

/* ── NVS profile storage ─────────────────────────────────────────── */

/* Save a custom button profile for the device identified by bda.
 * Only the map[] array is stored (GP_BTN_COUNT bytes).
 * The profile is loaded automatically on reconnect. */
esp_err_t gp_profile_save(const uint8_t *bda, const gp_profile_t *profile);

/* Load a previously saved profile for this bda.
 * Returns ESP_ERR_NOT_FOUND if no custom profile exists. */
esp_err_t gp_profile_load(const uint8_t *bda, gp_profile_t *out);

/* Delete a saved profile for this bda. */
esp_err_t gp_profile_delete(const uint8_t *bda);

/* Erase all saved profiles (call when clearing all bonds). */
esp_err_t gp_profiles_clear_all(void);
