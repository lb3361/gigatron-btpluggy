#include "gamepad.h"
#include "save.h"

#include "esp_log.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "GP";

/* ── d-pad source bitmask ─────────────────────────────────────────── */

#define DPAD_HAS_HAT     (1 << 0)   /* hat switch (usage 0x39) */
#define DPAD_HAS_BUTTONS (1 << 1)   /* direction usages 0x90-0x93 */
#define DPAD_HAS_AXES    (1 << 2)   /* X/Y axis thresholding */

/* ── report map layout (descriptor-derived or hardcoded) ─────────── */

typedef struct {
    /* buttons (usage page 0x09) */
    uint16_t btn_bit_offset;
    uint8_t  btn_count;          /* capped at 16 */

    /* d-pad sources (bitmask of DPAD_HAS_*) */
    uint8_t  dpad_sources;

    /* hat switch (usage page 0x01, usage 0x39) */
    uint16_t hat_bit_offset;
    uint8_t  hat_bit_size;
    int32_t  hat_logical_min;

    /* d-pad buttons (usage page 0x01, usages 0x90-0x93) */
    uint16_t dpad_up_offset;
    uint16_t dpad_down_offset;
    uint16_t dpad_right_offset;
    uint16_t dpad_left_offset;

    /* X/Y axes (usage page 0x01, usages 0x30/0x31) */
    uint16_t x_bit_offset;
    uint8_t  x_bit_size;
    int32_t  x_min, x_max;
    uint16_t y_bit_offset;
    uint8_t  y_bit_size;
    int32_t  y_min, y_max;
} gp_reportmap_t;

/* ── internal state ───────────────────────────────────────────────── */

struct gamepad_s {
    uint8_t  report_id;
    gp_reportmap_t rmap;

    /* profile */
    gp_profile_t profile;

    /* state */
    gp_state_t state;
    gp_state_t prev_state;

    /* callback */
    gp_event_cb_t cb;
    void         *user_data;
};

/* ── forward declarations (defined in customization section below) ── */

static const gp_profile_t *lookup_profile(uint16_t vid, uint16_t pid);

/* ── create / destroy ─────────────────────────────────────────────── */

gamepad_t *gamepad_create(const hid_field_map_t *map,
                          esp_hidh_dev_t *dev,
                          gp_event_cb_t cb, void *user_data)
{

    uint16_t vid = esp_hidh_dev_vendor_id_get(dev);
    uint16_t pid = esp_hidh_dev_product_id_get(dev);
    
    /* Search INPUT reports for button fields (usage page 0x09). */
    for (int r = 0; r < map->num_reports; r++) {
        const hid_report_desc_t *rd = &map->reports[r];
        if (rd->type != 1) continue;

        int btn_idx = -1;
        int hat_idx = -1;
        int dpad_up_idx = -1, dpad_down_idx = -1;
        int dpad_right_idx = -1, dpad_left_idx = -1;
        int x_idx = -1, y_idx = -1;

        for (int f = 0; f < rd->num_fields; f++) {
            const hid_field_t *fl = &rd->fields[f];

            /* buttons */
            if (fl->usage_page == 0x09 && (fl->flags & HID_FLAG_VARIABLE) &&
                !(fl->flags & HID_FLAG_CONSTANT)) {
                btn_idx = f;
            }
            /* hat switch */
            if (fl->usage_page == 0x01 && fl->usage == 0x39) {
                hat_idx = f;
            }
            /* d-pad direction buttons */
            if (fl->usage_page == 0x01) {
                if (fl->usage == 0x90) dpad_up_idx = f;
                if (fl->usage == 0x91) dpad_down_idx = f;
                if (fl->usage == 0x92) dpad_right_idx = f;
                if (fl->usage == 0x93) dpad_left_idx = f;
            }
            /* d-pad direction buttons via usage range */
            if (fl->usage_page == 0x01 && fl->usage <= 0x90 &&
                fl->usage_max >= 0x93 && fl->count >= 4) {
                dpad_up_idx = dpad_down_idx = dpad_right_idx = dpad_left_idx = f;
            }
            /* X/Y axes */
            if (fl->usage_page == 0x01 && fl->usage == 0x30) x_idx = f;
            if (fl->usage_page == 0x01 && fl->usage == 0x31) y_idx = f;
        }

        if (btn_idx < 0) continue;   /* no buttons → not a gamepad */

        gamepad_t *gp = calloc(1, sizeof(gamepad_t));
        if (!gp) return NULL;

        gp->report_id = rd->report_id;
        gp_reportmap_t *rm = &gp->rmap;

        /* buttons */
        rm->btn_bit_offset = rd->fields[btn_idx].bit_offset;
        rm->btn_count      = rd->fields[btn_idx].count;
        if (rm->btn_count > 16) rm->btn_count = 16;

        /* d-pad: populate ALL available sources */
        rm->dpad_sources = 0;

        if (hat_idx >= 0) {
            rm->dpad_sources   |= DPAD_HAS_HAT;
            rm->hat_bit_offset  = rd->fields[hat_idx].bit_offset;
            rm->hat_bit_size    = rd->fields[hat_idx].bit_size;
            rm->hat_logical_min = rd->fields[hat_idx].logical_min;
        }

        if (dpad_up_idx >= 0 && dpad_down_idx >= 0 &&
            dpad_right_idx >= 0 && dpad_left_idx >= 0) {
            rm->dpad_sources |= DPAD_HAS_BUTTONS;
            if (dpad_up_idx == dpad_down_idx) {
                /* All from one field with usage range 0x90..0x93 */
                const hid_field_t *fl = &rd->fields[dpad_up_idx];
                uint16_t base = fl->bit_offset;
                uint8_t  sz   = fl->bit_size;
                uint16_t off  = fl->usage;  /* usage_min */
                rm->dpad_up_offset    = base + (0x90 - off) * sz;
                rm->dpad_down_offset  = base + (0x91 - off) * sz;
                rm->dpad_right_offset = base + (0x92 - off) * sz;
                rm->dpad_left_offset  = base + (0x93 - off) * sz;
            } else {
                rm->dpad_up_offset    = rd->fields[dpad_up_idx].bit_offset;
                rm->dpad_down_offset  = rd->fields[dpad_down_idx].bit_offset;
                rm->dpad_right_offset = rd->fields[dpad_right_idx].bit_offset;
                rm->dpad_left_offset  = rd->fields[dpad_left_idx].bit_offset;
            }
        }

        if (x_idx >= 0 && y_idx >= 0) {
            rm->dpad_sources  |= DPAD_HAS_AXES;
            rm->x_bit_offset   = rd->fields[x_idx].bit_offset;
            rm->x_bit_size     = rd->fields[x_idx].bit_size;
            rm->x_min          = rd->fields[x_idx].logical_min;
            rm->x_max          = rd->fields[x_idx].logical_max;
            rm->y_bit_offset   = rd->fields[y_idx].bit_offset;
            rm->y_bit_size     = rd->fields[y_idx].bit_size;
            rm->y_min          = rd->fields[y_idx].logical_min;
            rm->y_max          = rd->fields[y_idx].logical_max;
        }

        /* profile + fixup */
        const gp_profile_t *prof = lookup_profile(vid, pid);
        memcpy(&gp->profile, prof, sizeof(gp_profile_t));
        gp->profile.name = prof->name;
        if (prof->fixup)
            prof->fixup(gp);
        
        /* saved button maps */
        int savedmap[GP_BTN_COUNT];
        if (nvs_load("GP", esp_hidh_dev_bda_get(dev), savedmap, GP_BTN_COUNT) == ESP_OK)
            memcpy(&gp->profile.map, savedmap, GP_BTN_COUNT);
        
        gp->cb        = cb;
        gp->user_data = user_data;

        /* log d-pad sources */
        char dpad_str[32] = "none";
        if (rm->dpad_sources) {
            char *p = dpad_str;
            if (rm->dpad_sources & DPAD_HAS_HAT)
                p += sprintf(p, "hat");
            if (rm->dpad_sources & DPAD_HAS_BUTTONS)
                p += sprintf(p, "%sbtns", p > dpad_str ? "+" : "");
            if (rm->dpad_sources & DPAD_HAS_AXES)
                p += sprintf(p, "%saxes", p > dpad_str ? "+" : "");
        }
        ESP_LOGI(TAG, "Gamepad decoder: report_id=%d  btns=%d  "
                 "dpad=%s  profile=%s",
                 gp->report_id, rm->btn_count, dpad_str,
                 gp->profile.name);
        return gp;
    }

    return NULL;
}

void gamepad_destroy(gamepad_t *gp)
{
    free(gp);
}

void gamepad_override_button_map(gamepad_t *gp,
                                 struct esp_hidh_dev_s *dev,
                                 int8_t map[GP_BTN_COUNT],
                                 bool save)
{
    if (gp) {
        memcpy(&gp->profile.map, map, GP_BTN_COUNT);
        if (save)
            nvs_save("GP", esp_hidh_dev_bda_get(dev), map, GP_BTN_COUNT);
    }
}


/* ── process report ───────────────────────────────────────────────── */

void gamepad_process_report(gamepad_t *gp,
                            esp_hidh_dev_t *dev,
                            uint8_t report_id,
                            const uint8_t *data, uint16_t len)
{
    if (!gp || report_id != gp->report_id) return;

    const gp_reportmap_t *rm = &gp->rmap;

    /* ---- buttons ---- */
    gp->state.raw_buttons = 0;
    for (int i = 0; i < rm->btn_count; i++) {
        uint16_t bit_pos = rm->btn_bit_offset + i;
        uint16_t byte_idx = bit_pos / 8;
        uint8_t  bit_idx  = bit_pos % 8;
        if (byte_idx < len && (data[byte_idx] & (1u << bit_idx)))
            gp->state.raw_buttons |= (1u << i);
    }

    /* apply profile mapping */
    for (int b = 0; b < GP_BTN_COUNT; b++) {
        int8_t idx = gp->profile.map[b];
        gp->state.buttons[b] = (idx >= 0 && idx < rm->btn_count)
                                ? !!(gp->state.raw_buttons & (1u << idx))
                                : false;
    }

    /* ---- d-pad: OR all available sources ---- */
    gp->state.dpad_up    = false;
    gp->state.dpad_down  = false;
    gp->state.dpad_left  = false;
    gp->state.dpad_right = false;

    if (rm->dpad_sources & DPAD_HAS_HAT) {
        int32_t raw = (int32_t)hid_extract_bits(
            data, len, rm->hat_bit_offset, rm->hat_bit_size);
        int32_t hat = raw - rm->hat_logical_min;
        switch (hat) {
        case 0: gp->state.dpad_up = true; break;
        case 1: gp->state.dpad_up = gp->state.dpad_right = true; break;
        case 2: gp->state.dpad_right = true; break;
        case 3: gp->state.dpad_down = gp->state.dpad_right = true; break;
        case 4: gp->state.dpad_down = true; break;
        case 5: gp->state.dpad_down = gp->state.dpad_left = true; break;
        case 6: gp->state.dpad_left = true; break;
        case 7: gp->state.dpad_up = gp->state.dpad_left = true; break;
        default: break; /* centered / null state */
        }
    }

    if (rm->dpad_sources & DPAD_HAS_BUTTONS) {
        gp->state.dpad_up    |= !!(hid_extract_bits(data, len, rm->dpad_up_offset, 1));
        gp->state.dpad_down  |= !!(hid_extract_bits(data, len, rm->dpad_down_offset, 1));
        gp->state.dpad_right |= !!(hid_extract_bits(data, len, rm->dpad_right_offset, 1));
        gp->state.dpad_left  |= !!(hid_extract_bits(data, len, rm->dpad_left_offset, 1));
    }

    if (rm->dpad_sources & DPAD_HAS_AXES) {
        hid_field_t xf = { .bit_offset = rm->x_bit_offset,
                            .bit_size   = rm->x_bit_size,
                            .logical_min = rm->x_min,
                            .logical_max = rm->x_max };
        hid_field_t yf = { .bit_offset = rm->y_bit_offset,
                            .bit_size   = rm->y_bit_size,
                            .logical_min = rm->y_min,
                            .logical_max = rm->y_max };
        int32_t x = hid_get_field_value(data, len, &xf, 0);
        int32_t y = hid_get_field_value(data, len, &yf, 0);
        int32_t x_range = rm->x_max - rm->x_min;
        int32_t y_range = rm->y_max - rm->y_min;
        int32_t x_lo = rm->x_min + x_range / 4;
        int32_t x_hi = rm->x_max - x_range / 4;
        int32_t y_lo = rm->y_min + y_range / 4;
        int32_t y_hi = rm->y_max - y_range / 4;
        gp->state.dpad_left  |= (x < x_lo);
        gp->state.dpad_right |= (x > x_hi);
        gp->state.dpad_up    |= (y < y_lo);
        gp->state.dpad_down  |= (y > y_hi);
    }

    /* ---- fire callback on change ---- */
    if (gp->cb && memcmp(&gp->state, &gp->prev_state, sizeof(gp_state_t)) != 0) {
        uint8_t gb = 0xff;
        if (gp->state.dpad_right)             { gb ^= 1; }
        if (gp->state.dpad_left)              { gb ^= 2; }
        if (gp->state.dpad_down)              { gb ^= 4; }
        if (gp->state.dpad_up)                { gb ^= 8; }
        if (gp->state.buttons[GP_BTN_START])  { gb ^= 16; }
        if (gp->state.buttons[GP_BTN_SELECT]) { gb ^= 32; }
        if (gp->state.buttons[GP_BTN_B])      { gb ^= 64; }
        if (gp->state.buttons[GP_BTN_A])      { gb ^= 128; }
        gp->state.giga_buttons = gb;
        gp->cb(&gp->state, gp->user_data);
    }
    gp->prev_state = gp->state;
}

/* ── query ────────────────────────────────────────────────────────── */

const gp_state_t *gamepad_get_state(const gamepad_t *gp)
{
    return gp ? &gp->state : NULL;
}


/* ╔════════════════════════════════════════════════════════════════════╗
 * ║  DEVICE-SPECIFIC CUSTOMIZATIONS                                    ║
 * ║                                                                    ║
 * ║  Everything below this banner is device-specific:                  ║
 * ║  - Fixup functions (adjust rmap/profile after descriptor parse)    ║
 * ║  - Built-in profiles (button index mappings)                       ║
 * ║  - VID/PID table (maps vendor/product to a profile)                ║
 * ║                                                                    ║
 * ║  To add support for a new device:                                  ║
 * ║  1. If needed, write a fixup function to patch rmap or profile     ║
 * ║  2. Define or reuse a gp_profile_t with the button map             ║
 * ║  3. Add a VID/PID entry pointing to the profile                    ║
 * ║                                                                    ║
 * ║  Button index = HID Button usage - 1 (0-based).                    ║
 * ║  GP_BTN_A/B use label-based convention: GP_BTN_A maps to the       ║
 * ║  button the manufacturer labels "A" (or equivalent primary).       ║
 * ║                                                                    ║
 * ║  Devices with fully proprietary protocols (Switch full mode,       ║
 * ║  DS3, Wii, Steam Controller) cannot be decoded through the         ║
 * ║  standard HID report parser and are not handled here.              ║
 * ╚════════════════════════════════════════════════════════════════════╝ */

/* ── Generic (fallback for unknown devices) ──────────────────────── */

/* Common dinput layout (many third-party and Android gamepads):
 *   Btn1=A Btn2=B Btn3=X Btn4=Y L1=5 R1=6 L2=7 R2=8
 *   Select=9 Start=10 L3=11 R3=12
 * Also works for: Google Stadia (standard HID, Android-like). */
const gp_profile_t GP_PROFILE_GENERIC = {
    .name = "Generic",
    .map  = { [GP_BTN_A] = 0, [GP_BTN_B] = 1,
              [GP_BTN_START] = 8, [GP_BTN_SELECT] = 9 },
};

/* ── Xbox ─────────────────────────────────────────────────────────── */

/*  Three firmware variants with different HID button layouts:
 *
 *  v3.1 (BT Classic, descriptor ≤ 330 bytes): 10 buttons
 *    A=0 B=1 X=2 Y=3 LB=4 RB=5 View=6 Menu=7 LS=8 RS=9
 *
 *  v4.8 (BT Classic, descriptor > 330 bytes): 15 buttons
 *    A=0 B=1 _=2 X=3 Y=4 _=5 LB=6 RB=7 _=8 _=9 View=10 Menu=11
 *    Guide=12 LS=13 RS=14
 *    (On v4.8, Select/View may arrive as Consumer Page AC_BACK
 *     rather than a Button page usage.)
 *
 *  v5.x (BLE): same button indices as v4.8.
 *
 *  D-pad: hat switch or individual dpad usages — both supported
 *  by our descriptor parser.
 *
 *  Profile defaults to v3.1.  Fixup detects v4.8+ via btn_count.
 *  Also covers: Nvidia Shield, SteelSeries, Amazon Luna. */
static void xbox_fixup(gamepad_t *gp)
{
    if (gp->rmap.btn_count > 10) {
        gp->profile.map[GP_BTN_START]  = 11;
        gp->profile.map[GP_BTN_SELECT] = 10;
        ESP_LOGI(TAG, "Xbox: adjusted to v4.8+ mapping (%d buttons)",
                 gp->rmap.btn_count);
    }
}

const gp_profile_t GP_PROFILE_XBOX = {
    .name  = "Xbox",
    .map   = { [GP_BTN_A] = 0, [GP_BTN_B] = 1,
               [GP_BTN_START] = 7, [GP_BTN_SELECT] = 6 },
    .fixup = xbox_fixup,
};

/* ── PlayStation ──────────────────────────────────────────────────── */

/*  DS4/DS5 over BT Classic present a standard HID descriptor.
 *  The proprietary byte-level format (report 0x11/0x31 used by
 *  bluepad32) is NOT what we see — esp_hidh delivers parsed reports
 *  matching the HID descriptor.
 *
 *  HID descriptor button order (DS4 & DS5):
 *    Square=0 Cross=1 Circle=2 Triangle=3
 *    L1=4 R1=5 L2=6 R2=7
 *    Share=8 Options=9 L3=10 R3=11 PS=12 Touchpad=13
 *
 *  D-pad: hat switch (4 bits, values 0-7 for 8 directions).
 *
 *  Note: DS3/Sixaxis requires a magic "enable reports" packet
 *  and will NOT produce input through standard esp_hidh. */
const gp_profile_t GP_PROFILE_PLAYSTATION = {
    .name = "PlayStation",
    .map  = { [GP_BTN_A] = 1, [GP_BTN_B] = 2,
              [GP_BTN_START] = 9, [GP_BTN_SELECT] = 8 },
};

/* ── Nintendo ─────────────────────────────────────────────────────── */

/*  Switch Pro / Joy-Con in standard HID mode (report 0x3F):
 *    B=0(South) A=1(East) Y=2(North) X=3(West)
 *    L=4 R=5 ZL=6 ZR=7
 *    -=8(Select) +=9(Start) L3=10 R3=11 Home=12 Capture=13
 *
 *  D-pad: hat switch in standard HID mode (report 0x3F).
 *         In full-report mode (report 0x30), d-pad uses individual
 *         bits in a proprietary format — we cannot handle that.
 *
 *  Note: Nintendo labels A(East) and B(South), opposite to Xbox.
 *  This profile uses label-based mapping: GP_BTN_A = the button
 *  labeled "A" on the controller (East position, index 1).
 *
 *  Also covers: HORI pads, PDP controllers (Nintendo-licensed). */
const gp_profile_t GP_PROFILE_NINTENDO = {
    .name = "Nintendo",
    .map  = { [GP_BTN_A] = 1, [GP_BTN_B] = 0,
              [GP_BTN_START] = 9, [GP_BTN_SELECT] = 8 },
};

/* ── 8BitDo ───────────────────────────────────────────────────────── */

/* Android/dinput mode (the mode that uses 8BitDo's own VID):
 *   Btn1=B(South) Btn2=A(East) _=3 Btn4=Y Btn5=X
 *   _=6 L=7 R=8 ZL=9 ZR=10 Select=11 Start=12
 *
 *   A/B and X/Y are swapped vs Xbox because 8BitDo uses
 *   Nintendo physical layout (A=East, B=South) but reports
 *   standard HID button indices.
 *
 *   In Switch mode, 8BitDo spoofs Nintendo VID 057E:2009 and
 *   is handled by the Nintendo profile.
 *   In macOS/Xbox mode, it spoofs an Xbox controller.
 *
 *   D-pad: hat switch or individual dpad usages (varies by
 *   model and firmware). Both are handled by our parser.
 *
 *   8BitDo Zero 2 in keyboard mode sends HID Keyboard page
 *   reports — handled by the keyboard decoder, not here. */
const gp_profile_t GP_PROFILE_8BITDO = {
    .name = "8BitDo",
    .map  = { [GP_BTN_A] = 0, [GP_BTN_B] = 1,
              [GP_BTN_START] = 11, [GP_BTN_SELECT] = 10 },
};

/* ── VID/PID table ────────────────────────────────────────────────── */

typedef struct {
    uint16_t vid;
    uint16_t pid;
    const gp_profile_t *profile;
} vid_pid_entry_t;

/* PID 0x0000 is a catch-all: matches any PID for that VID.
 * Exact PID entries are checked first (two-pass lookup). */
static const vid_pid_entry_t s_vid_pid_table[] = {
    /* ── VID-only catch-alls (all PIDs for this vendor) ──────────────── */
    { 0x045E, 0x0000, &GP_PROFILE_XBOX },       /* Microsoft */
    { 0x054C, 0x0000, &GP_PROFILE_PLAYSTATION }, /* Sony (DS4/DS5) */
    { 0x057E, 0x0000, &GP_PROFILE_NINTENDO },    /* Nintendo */
    { 0x2DC8, 0x0000, &GP_PROFILE_8BITDO },      /* 8BitDo */
    { 0x2820, 0x0000, &GP_PROFILE_8BITDO },      /* 8BitDo (old VID) */
    { 0x0955, 0x0000, &GP_PROFILE_XBOX },        /* Nvidia (Shield) */
    { 0x1038, 0x0000, &GP_PROFILE_XBOX },        /* SteelSeries (Nimbus etc) */
    { 0x0F0D, 0x0000, &GP_PROFILE_NINTENDO },    /* HORI (Nintendo pads) */
    { 0x18D1, 0x0000, &GP_PROFILE_GENERIC },     /* Google (Stadia) */
    { 0x1532, 0x0000, &GP_PROFILE_XBOX },        /* Razer */
    { 0x24C6, 0x0000, &GP_PROFILE_XBOX },        /* PowerA (Xbox pads) */

    /* ── PID-specific overrides (3rd-party VIDs with mixed profiles) ── */
    { 0x0E6F, 0x0186, &GP_PROFILE_NINTENDO },    /* PDP Afterglow Wireless */
    { 0x1949, 0x041A, &GP_PROFILE_XBOX },        /* Amazon Luna */
    /* Note: 8BitDo in Switch mode spoofs 057E:2009 (Nintendo catch-all) */
};

#define VID_PID_TABLE_SIZE \
    (sizeof(s_vid_pid_table) / sizeof(s_vid_pid_table[0]))

static const gp_profile_t *lookup_profile(uint16_t vid, uint16_t pid)
{
    /* Pass 1: exact VID+PID match */
    for (int i = 0; i < (int)VID_PID_TABLE_SIZE; i++) {
        if (s_vid_pid_table[i].vid == vid &&
            s_vid_pid_table[i].pid != 0 &&
            s_vid_pid_table[i].pid == pid)
            return s_vid_pid_table[i].profile;
    }
    /* Pass 2: VID-only catch-all (pid == 0x0000) */
    for (int i = 0; i < (int)VID_PID_TABLE_SIZE; i++) {
        if (s_vid_pid_table[i].vid == vid &&
            s_vid_pid_table[i].pid == 0)
            return s_vid_pid_table[i].profile;
    }
    return &GP_PROFILE_GENERIC;
}

/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
