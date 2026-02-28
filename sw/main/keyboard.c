#include "keyboard.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "KB";

#define KB_MAX_KEYS       10
#define KB_ROLLOVER_CODE  0x01   /* phantom / rollover error */

struct keyboard_s {
    uint8_t  report_id;

    /* modifier field (Variable, 1 bit × 8, usage page 0x07) */
    uint16_t mod_bit_offset;

    /* key array field (Array, N × 8-bit scan codes, usage page 0x07) */
    uint16_t key_bit_offset;
    uint8_t  key_bit_size;
    uint8_t  key_count;

    /* state */
    uint8_t  prev_modifiers;
    uint8_t  prev_keys[KB_MAX_KEYS];
    uint8_t  curr_keys[KB_MAX_KEYS];
    uint8_t  curr_key_count;

    /* callback */
    kb_event_cb_t cb;
    void         *user_data;
};

/* ── helpers ──────────────────────────────────────────────────────── */

static bool key_in_array(uint8_t key, const uint8_t *arr, int count)
{
    for (int i = 0; i < count; i++)
        if (arr[i] == key) return true;
    return false;
}

/* ── create / destroy ─────────────────────────────────────────────── */

keyboard_t *keyboard_create(const hid_field_map_t *map,
                             kb_event_cb_t cb, void *user_data)
{
    /* Search INPUT reports for keyboard fields (usage page 0x07). */
    for (int r = 0; r < map->num_reports; r++) {
        const hid_report_desc_t *rd = &map->reports[r];
        if (rd->type != 1) continue;             /* INPUT only */

        int mod_idx = -1, key_idx = -1;

        for (int f = 0; f < rd->num_fields; f++) {
            const hid_field_t *fl = &rd->fields[f];
            if (fl->usage_page != 0x07)  continue;
            if (fl->flags & HID_FLAG_CONSTANT) continue;

            if ((fl->flags & HID_FLAG_VARIABLE) &&
                ((fl->usage >= 0xE0 && fl->usage <= 0xE7) ||
                 (fl->usage <= 0xE0 && fl->usage_max >= 0xE7))) {
                mod_idx = f;
            } else if (!(fl->flags & HID_FLAG_VARIABLE)) {
                /* Array type → key-code array */
                key_idx = f;
            }
        }

        if (mod_idx >= 0 && key_idx >= 0) {
            keyboard_t *kb = calloc(1, sizeof(keyboard_t));
            if (!kb) return NULL;

            kb->report_id      = rd->report_id;
            kb->mod_bit_offset = rd->fields[mod_idx].bit_offset;
            kb->key_bit_offset = rd->fields[key_idx].bit_offset;
            kb->key_bit_size   = rd->fields[key_idx].bit_size;
            kb->key_count      = rd->fields[key_idx].count;
            if (kb->key_count > KB_MAX_KEYS)
                kb->key_count = KB_MAX_KEYS;
            kb->cb        = cb;
            kb->user_data = user_data;

            ESP_LOGI(TAG, "Keyboard decoder: report_id=%d "
                     "mod@bit%d  keys@bit%d ×%d",
                     kb->report_id, kb->mod_bit_offset,
                     kb->key_bit_offset, kb->key_count);
            return kb;
        }
    }

    return NULL;
}

void keyboard_destroy(keyboard_t *kb)
{
    free(kb);
}

/* ── process report ───────────────────────────────────────────────── */

void keyboard_process_report(keyboard_t *kb,
                              uint8_t report_id,
                              const uint8_t *data, uint16_t len)
{
    if (!kb || report_id != kb->report_id) return;

    /* Extract modifier byte (8 bits starting at mod_bit_offset) */
    uint8_t modifiers = (uint8_t)hid_extract_bits(
        data, len, kb->mod_bit_offset, 8);

    /* Extract key array */
    kb->curr_key_count = 0;
    for (int i = 0; i < kb->key_count; i++) {
        uint8_t code = (uint8_t)hid_extract_bits(
            data, len,
            kb->key_bit_offset + i * kb->key_bit_size,
            kb->key_bit_size);
        if (code == KB_ROLLOVER_CODE) return;   /* rollover — ignore */
        if (code != 0)
            kb->curr_keys[kb->curr_key_count++] = code;
    }

    if (!kb->cb) goto save;

    /* Modifier changes → emit events for each changed bit */
    uint8_t mod_diff = modifiers ^ kb->prev_modifiers;
    for (int i = 0; i < 8; i++) {
        if (!(mod_diff & (1 << i))) continue;
        kb_event_t ev = {
            .type      = (modifiers & (1 << i)) ? KB_KEY_DOWN : KB_KEY_UP,
            .scancode  = 0xE0 + i,
            .modifiers = modifiers,
        };
        kb->cb(&ev, kb->user_data);
    }

    /* Released keys: in prev but not in curr */
    for (int i = 0; i < KB_MAX_KEYS; i++) {
        uint8_t k = kb->prev_keys[i];
        if (k == 0) continue;
        if (!key_in_array(k, kb->curr_keys, kb->curr_key_count)) {
            kb_event_t ev = {
                .type      = KB_KEY_UP,
                .scancode  = k,
                .modifiers = modifiers,
            };
            kb->cb(&ev, kb->user_data);
        }
    }

    /* Pressed keys: in curr but not in prev */
    for (int i = 0; i < kb->curr_key_count; i++) {
        uint8_t k = kb->curr_keys[i];
        if (!key_in_array(k, kb->prev_keys, KB_MAX_KEYS)) {
            kb_event_t ev = {
                .type      = KB_KEY_DOWN,
                .scancode  = k,
                .modifiers = modifiers,
            };
            kb->cb(&ev, kb->user_data);
        }
    }

save:
    kb->prev_modifiers = modifiers;
    memset(kb->prev_keys, 0, sizeof(kb->prev_keys));
    memcpy(kb->prev_keys, kb->curr_keys, kb->curr_key_count);
}

/* ── query ────────────────────────────────────────────────────────── */

uint8_t keyboard_get_modifiers(const keyboard_t *kb)
{
    return kb ? kb->prev_modifiers : 0;
}

const uint8_t *keyboard_get_pressed_keys(const keyboard_t *kb, int *count)
{
    if (!kb) { *count = 0; return NULL; }
    /* Count non-zero entries in prev_keys */
    int n = 0;
    for (int i = 0; i < KB_MAX_KEYS && kb->prev_keys[i]; i++) n++;
    *count = n;
    return kb->prev_keys;
}
