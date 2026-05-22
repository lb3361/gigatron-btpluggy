#include "keyboard.h"
#include "save.h"

#include "esp_log.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"

#include <stdlib.h>
#include <string.h>

#include <keymap.h>

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

    /* led field (variable, 1 bit x 8, usage page 0x08) */
    int16_t  led_report_id;
    uint16_t capslock_bit_offset;

    /* state */
    uint8_t  prev_modifiers;
    uint8_t  prev_keys[KB_MAX_KEYS];
    uint8_t  curr_keys[KB_MAX_KEYS];
    uint8_t  curr_key_count;

    /* keymap state */
    uint8_t  keymap;
    uint8_t  capslock;

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

static void set_led(keyboard_t *kb, esp_hidh_dev_t *dev, bool capslock)
{
    if (kb && kb->led_report_id >= 0) {
        uint8_t report = (capslock) ? (1 << kb->capslock_bit_offset) : 0;
        esp_hidh_dev_output_set(dev, 0, kb->led_report_id, &report, 1);
    }
}


/* ── create / destroy ─────────────────────────────────────────────── */

keyboard_t *keyboard_create(const hid_field_map_t *map,
                            esp_hidh_dev_t *dev,
                            kb_event_cb_t cb, void *user_data)
{
    keyboard_t *kb = 0;
    
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
                (fl->usage == 0xE0 && fl->bit_size == 1 && fl->count >= 8) )
                mod_idx = f;
            else if (!(fl->flags & HID_FLAG_VARIABLE) &&
                     (fl->usage == 0x00) )
                key_idx = f;
        }

        if (mod_idx >= 0 && key_idx >= 0) {
            kb = calloc(1, sizeof(keyboard_t));
            if (!kb) return NULL;
            kb->led_report_id  = -1;
            kb->report_id      = rd->report_id;
            kb->mod_bit_offset = rd->fields[mod_idx].bit_offset;
            kb->key_bit_offset = rd->fields[key_idx].bit_offset;
            kb->key_bit_size   = rd->fields[key_idx].bit_size;
            kb->key_count      = rd->fields[key_idx].count;
            if (kb->key_count > KB_MAX_KEYS)
                kb->key_count = KB_MAX_KEYS;
            kb->cb        = cb;
            kb->user_data = user_data;
            
            /* load saved keymap */
            const uint8_t *bda = esp_hidh_dev_bda_get(dev);
            if (bda) {
                uint8_t keymap;
                if (nvs_load("KB", bda, &keymap, 1) == ESP_OK) {
                    ESP_LOGI(TAG, "Setting saved keymap %d", keymap);
                    kb->keymap = keymap;
                }
            }

            ESP_LOGI(TAG, "Keyboard decoder: report_id=%d "
                     "mod@bit%d  keys@bit%d ×%d",
                     kb->report_id, kb->mod_bit_offset,
                     kb->key_bit_offset, kb->key_count);
            break;
        }
    }

    /* Search OUTPUT reports for leds */
    if (kb == NULL)
        return 0;
    for (int r = 0; r < map->num_reports; r++) {
        const hid_report_desc_t *rd = &map->reports[r];
        if (rd->type != 2) continue;             /* OUTPUT only */
        if (rd->total_bits != 8) continue;       /* one byte only */
        for (int f = 0; f < rd->num_fields; f++) {
            const hid_field_t *fl = &rd->fields[f];
            if (fl->usage_page != 0x08)  continue;
            if (fl->bit_size != 1) continue;
            if (fl->usage <= 0x2 && fl->usage_max >= 0x2) {
                kb->led_report_id = rd->report_id;
                kb->capslock_bit_offset = fl->bit_offset + 0x2 - fl->usage;
                break;
            }
        }
    }

    /* Clear Leds (including caps_lock) */
    if (kb && kb->led_report_id >= 0)
        set_led(kb, dev, 0);

    return kb;
}

void keyboard_destroy(keyboard_t *kb)
{
    free(kb);
}


/* ── keymaps ───────────────────────────────────────────────── */

static int lookup(uint8_t mods, uint8_t code)
{
    ESP_LOGD(TAG, "Keymap-to-ascii %02x %02x", mods, code);
    for (int i=0; i<sizeof(keymaps)/sizeof(keymaps[0]); i++) {
        const keyTuple_t *t = &keymaps[i];
        uint8_t f = t->flags;
        if (code == t->code)
            if (mods & t->flags & KMAPS) {
                f |= KMAPS;
                if (f == (EVERY|KMAPS) || f == (mods|KMAPS))
                    return t->ascii;
            }
    }
    return -1;
}


void process_giga_keys(keyboard_t *kb,
                       esp_hidh_dev_t *dev,
                       kb_event_t *ev)
{
    ev->giga_buttons = 0xff;
    ev->giga_key = 0xff;

    /* caps lock */
    if (ev->type == KB_KEY_DOWN && ev->scancode == 0x39) {
        kb->capslock = ! kb->capslock;
        set_led(kb, dev, kb->capslock);
    }
    
    /* mods */
    int  ascii = -1;
    bool shift = (ev->modifiers & 0x22) != 0;
    bool altgr = (ev->modifiers & 0x40) != 0;
    bool ctrl =  (ev->modifiers & 0x11) != 0;
    bool alt =   (ev->modifiers & 0x44) != 0;
    uint8_t mods = (1 << kb->keymap);
    if (kb->capslock && (kb->keymap == 2 || kb->keymap == 3))
        shift = true;
    if (altgr)
        ascii = lookup(mods+ALTGR, ev->scancode);
    if (shift)
        mods += SHIFT;
    if (ascii < 0)
        ascii = lookup(mods, ev->scancode);
    
    /* ctrl */
    if (ascii >= 0) {
        if (ctrl && alt && ascii >= 193 && ascii <= 204) { // CTRL+ALT+Fn
            // switch keymap
            if (ascii < 193 + nrKeymaps) {
                kb->keymap = ascii - 193;
                const uint8_t *bda = esp_hidh_dev_bda_get(dev);
                ESP_LOGI(TAG, "Switch to keymap %d", kb->keymap);
                if (bda)
                    nvs_save("KB", bda, &kb->keymap, 1);
            }
#if CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
            if (ascii == 204) {
                char *buffer = malloc(1024);
                if (buffer) {
                    vTaskList(buffer);
                    ESP_LOGI(TAG, "Task list: \n%s", buffer);
                    free(buffer);
                }
                esp_intr_dump(stdout);
            }
#endif
            ascii = -1;
        } else {
            if (kb->capslock && ascii >= 'a' && ascii <= 'z')
                ascii += 'A' - 'a';
            else if (ctrl) {
                if (ascii == '?')
                    ascii = 127;
                else if (ascii == '6' || ascii == '^' || ascii == ' ')
                    ascii = 0;
                else if ((ascii|0x20) >= 'a' && (ascii|0x20) <= 'z')
                    ascii &= 31;
            }
        }
    }
    if (ascii >= 0) 
        ev->giga_key = (uint8_t)ascii;
    
    /* buttons */
    uint8_t btns = 0;
    uint8_t ctrlaltdel = 0;
    for (int i=0; i<kb->curr_key_count; i++) {
        uint8_t scan = kb->curr_keys[i];
        for (int j=0; j<sizeof(keybtns)/sizeof(keybtns[0]); j++)
            if (scan == keybtns[j].code)
                btns |= keybtns[j].btn;
        if (scan == 0x4C && ctrl && alt)
            ctrlaltdel = 1;
    }
    if (ctrlaltdel && btns == 128) {
        ev->giga_buttons = 16 ^ 0xff;
        ev->giga_key = 16 ^ 0xff;
    } else if (btns) {
        ev->giga_buttons = btns ^ 0xff;
        ev->giga_key = 0xff;
    }
    /* Callback */
    if (kb->cb)
        kb->cb(ev, kb->user_data);
}
                             

/* ── process report ───────────────────────────────────────────────── */


void keyboard_process_report(keyboard_t *kb,
                             esp_hidh_dev_t *dev,
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

    /* Modifier changes → emit events for each changed bit */
    uint8_t mod_diff = modifiers ^ kb->prev_modifiers;
    for (int i = 0; i < 8; i++) {
        if (!(mod_diff & (1 << i))) continue;
        kb_event_t ev = {
            .type      = (modifiers & (1 << i)) ? KB_KEY_DOWN : KB_KEY_UP,
            .scancode  = 0xE0 + i,
            .modifiers = modifiers,
        };
        process_giga_keys(kb, dev, &ev);
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
            process_giga_keys(kb, dev, &ev);
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
            process_giga_keys(kb, dev, &ev);
        }
    }
    

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



/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
