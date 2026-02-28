#include "hid_parser.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HID_PARSE";

/* ── HID item encoding constants ──────────────────────────────────── */

/* Item type (bits 3:2 of prefix byte) */
#define ITEM_TYPE_MAIN   0
#define ITEM_TYPE_GLOBAL 1
#define ITEM_TYPE_LOCAL  2

/* Main-item tags (bits 7:4) */
#define TAG_INPUT          0x08
#define TAG_OUTPUT         0x09
#define TAG_FEATURE        0x0B
#define TAG_COLLECTION     0x0A
#define TAG_END_COLLECTION 0x0C

/* Global-item tags */
#define TAG_USAGE_PAGE     0x00
#define TAG_LOGICAL_MIN    0x01
#define TAG_LOGICAL_MAX    0x02
#define TAG_REPORT_SIZE    0x07
#define TAG_REPORT_ID      0x08
#define TAG_REPORT_COUNT   0x09
#define TAG_PUSH           0x0A
#define TAG_POP            0x0B

/* Local-item tags */
#define TAG_USAGE          0x00
#define TAG_USAGE_MIN      0x01
#define TAG_USAGE_MAX      0x02

/* ── parser state ─────────────────────────────────────────────────── */

#define MAX_USAGES  32
#define MAX_PUSH     4

typedef struct {
    uint16_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max;
    uint16_t report_size;
    uint16_t report_count;
    uint8_t  report_id;
} global_state_t;

typedef struct {
    uint32_t usages[MAX_USAGES]; /* upper 16 = page override (0 = use global) */
    uint8_t  num_usages;
    uint32_t usage_min;          /* extended: (page << 16) | usage_id */
    uint32_t usage_max;
    bool     has_usage_range;
} local_state_t;

/* ── helpers ──────────────────────────────────────────────────────── */

static uint32_t get_udata(const uint8_t *d, uint8_t sz)
{
    switch (sz) {
    case 1: return d[0];
    case 2: return d[0] | ((uint32_t)d[1] << 8);
    case 4: return d[0] | ((uint32_t)d[1] << 8) |
                   ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    default: return 0;
    }
}

static int32_t get_sdata(const uint8_t *d, uint8_t sz)
{
    switch (sz) {
    case 1: return (int8_t)d[0];
    case 2: return (int16_t)(d[0] | (d[1] << 8));
    case 4: return (int32_t)(d[0] | ((uint32_t)d[1] << 8) |
                   ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24));
    default: return 0;
    }
}

/* Find or create a report entry for the given (report_id, type). */
static hid_report_desc_t *get_report(hid_field_map_t *map,
                                      uint8_t report_id, uint8_t type)
{
    for (int i = 0; i < map->num_reports; i++) {
        if (map->reports[i].report_id == report_id &&
            map->reports[i].type == type)
            return &map->reports[i];
    }
    if (map->num_reports >= HID_MAX_REPORTS) return NULL;
    hid_report_desc_t *r = &map->reports[map->num_reports++];
    r->report_id  = report_id;
    r->type       = type;
    r->total_bits = 0;
    r->num_fields = 0;
    return r;
}

/* Extract usage page and usage ID from a possibly-extended usage value.
 * If the upper 16 bits are set, they override the global usage page. */
static uint16_t usage_page_of(uint32_t ext_usage, uint16_t global_page)
{
    uint16_t p = (uint16_t)(ext_usage >> 16);
    return p ? p : global_page;
}

static uint16_t usage_id_of(uint32_t ext_usage)
{
    return (uint16_t)(ext_usage & 0xFFFF);
}

/* Emit field(s) for one INPUT / OUTPUT / FEATURE main item. */
static void add_field(hid_report_desc_t *report,
                       const global_state_t *gs,
                       const local_state_t *ls,
                       uint8_t flags)
{
    if (!report) return;

    uint16_t total_bits = gs->report_size * gs->report_count;

    if (ls->has_usage_range) {
        /* usage_min .. usage_max → one field with full count */
        if (report->num_fields < HID_MAX_FIELDS) {
            hid_field_t *f = &report->fields[report->num_fields++];
            f->usage_page  = usage_page_of(ls->usage_min, gs->usage_page);
            f->usage       = usage_id_of(ls->usage_min);
            f->usage_max   = usage_id_of(ls->usage_max);
            f->logical_min = gs->logical_min;
            f->logical_max = gs->logical_max;
            f->bit_offset  = report->total_bits;
            f->bit_size    = gs->report_size;
            f->count       = gs->report_count;
            f->flags       = flags;
        }
    } else if (ls->num_usages > 1 && (flags & HID_FLAG_VARIABLE)) {
        /* Multiple individual usages + Variable → one field per usage */
        uint16_t n = ls->num_usages < gs->report_count
                     ? ls->num_usages : gs->report_count;
        for (uint16_t i = 0; i < n && report->num_fields < HID_MAX_FIELDS; i++) {
            hid_field_t *f = &report->fields[report->num_fields++];
            f->usage_page  = usage_page_of(ls->usages[i], gs->usage_page);
            f->usage       = usage_id_of(ls->usages[i]);
            f->usage_max   = 0;
            f->logical_min = gs->logical_min;
            f->logical_max = gs->logical_max;
            f->bit_offset  = report->total_bits + i * gs->report_size;
            f->bit_size    = gs->report_size;
            f->count       = 1;
            f->flags       = flags;
        }
    } else {
        /* Single or no usage → one field */
        if (report->num_fields < HID_MAX_FIELDS) {
            hid_field_t *f = &report->fields[report->num_fields++];
            f->usage_page  = ls->num_usages > 0
                             ? usage_page_of(ls->usages[0], gs->usage_page)
                             : gs->usage_page;
            f->usage       = ls->num_usages > 0
                             ? usage_id_of(ls->usages[0]) : 0;
            f->usage_max   = 0;
            f->logical_min = gs->logical_min;
            f->logical_max = gs->logical_max;
            f->bit_offset  = report->total_bits;
            f->bit_size    = gs->report_size;
            f->count       = gs->report_count;
            f->flags       = flags;
        }
    }

    report->total_bits += total_bits;
}

static void clear_local(local_state_t *ls)
{
    ls->num_usages    = 0;
    ls->usage_min     = 0;
    ls->usage_max     = 0;
    ls->has_usage_range = false;
}

/* ── public: parse ────────────────────────────────────────────────── */

esp_err_t hid_parse_report_map(const uint8_t *desc, size_t len,
                                hid_field_map_t *out)
{
    memset(out, 0, sizeof(*out));

    global_state_t gs = {0};
    local_state_t  ls = {0};
    global_state_t push_stack[MAX_PUSH];
    int push_depth = 0;

    size_t pos = 0;
    while (pos < len) {
        uint8_t prefix = desc[pos];

        /* Long item (0xFE) — skip */
        if (prefix == 0xFE) {
            if (pos + 1 >= len) break;
            pos += 3 + desc[pos + 1];
            continue;
        }

        uint8_t bSize = prefix & 0x03;
        uint8_t bType = (prefix >> 2) & 0x03;
        uint8_t bTag  = (prefix >> 4) & 0x0F;
        uint8_t data_size = (bSize == 3) ? 4 : bSize;

        if (pos + 1 + data_size > len) break;
        const uint8_t *item_data = &desc[pos + 1];

        switch (bType) {

        case ITEM_TYPE_GLOBAL:
            switch (bTag) {
            case TAG_USAGE_PAGE:
                gs.usage_page = (uint16_t)get_udata(item_data, data_size);
                break;
            case TAG_LOGICAL_MIN:
                gs.logical_min = get_sdata(item_data, data_size);
                break;
            case TAG_LOGICAL_MAX:
                gs.logical_max = get_sdata(item_data, data_size);
                break;
            case TAG_REPORT_SIZE:
                gs.report_size = (uint16_t)get_udata(item_data, data_size);
                break;
            case TAG_REPORT_COUNT:
                gs.report_count = (uint16_t)get_udata(item_data, data_size);
                break;
            case TAG_REPORT_ID:
                gs.report_id = (uint8_t)get_udata(item_data, data_size);
                break;
            case TAG_PUSH:
                if (push_depth < MAX_PUSH)
                    push_stack[push_depth++] = gs;
                break;
            case TAG_POP:
                if (push_depth > 0)
                    gs = push_stack[--push_depth];
                break;
            }
            break;

        case ITEM_TYPE_LOCAL:
            switch (bTag) {
            case TAG_USAGE: {
                uint32_t v = get_udata(item_data, data_size);
                if (ls.num_usages < MAX_USAGES)
                    ls.usages[ls.num_usages++] = v;
                /* 4-byte extended usage: upper 16 bits carry a usage page.
                 * Propagate to global state so that subsequent 2-byte usages
                 * (which rely on the global page) inherit it.  This handles
                 * descriptors that set the page only via extended usages
                 * before a Collection, without an explicit Usage Page item. */
                if (data_size == 4 && (v >> 16) != 0)
                    gs.usage_page = (uint16_t)(v >> 16);
                break;
            }
            case TAG_USAGE_MIN: {
                uint32_t v = get_udata(item_data, data_size);
                ls.usage_min = v;
                ls.has_usage_range = true;
                if (data_size == 4 && (v >> 16) != 0)
                    gs.usage_page = (uint16_t)(v >> 16);
                break;
            }
            case TAG_USAGE_MAX: {
                uint32_t v = get_udata(item_data, data_size);
                ls.usage_max = v;
                ls.has_usage_range = true;
                if (data_size == 4 && (v >> 16) != 0)
                    gs.usage_page = (uint16_t)(v >> 16);
                break;
            }
            }
            break;

        case ITEM_TYPE_MAIN:
            switch (bTag) {
            case TAG_INPUT: {
                uint8_t flags = data_size > 0 ? item_data[0] : 0;
                hid_report_desc_t *r = get_report(out, gs.report_id, 1);
                add_field(r, &gs, &ls, flags);
                clear_local(&ls);
                break;
            }
            case TAG_OUTPUT: {
                uint8_t flags = data_size > 0 ? item_data[0] : 0;
                hid_report_desc_t *r = get_report(out, gs.report_id, 2);
                add_field(r, &gs, &ls, flags);
                clear_local(&ls);
                break;
            }
            case TAG_FEATURE: {
                uint8_t flags = data_size > 0 ? item_data[0] : 0;
                hid_report_desc_t *r = get_report(out, gs.report_id, 3);
                add_field(r, &gs, &ls, flags);
                clear_local(&ls);
                break;
            }
            case TAG_COLLECTION:
            case TAG_END_COLLECTION:
                clear_local(&ls);
                break;
            }
            break;
        }

        pos += 1 + data_size;
    }

    ESP_LOGI(TAG, "Parsed %d report(s) from %d-byte descriptor",
             out->num_reports, (int)len);
    return ESP_OK;
}

/* ── public: bit extraction ───────────────────────────────────────── */

uint32_t hid_extract_bits(const uint8_t *data, uint16_t data_len,
                           uint16_t bit_offset, uint8_t bit_size)
{
    uint32_t value = 0;
    for (int i = 0; i < bit_size && i < 32; i++) {
        uint16_t byte_idx = (bit_offset + i) / 8;
        uint8_t  bit_idx  = (bit_offset + i) % 8;
        if (byte_idx < data_len && (data[byte_idx] & (1u << bit_idx)))
            value |= (1u << i);
    }
    return value;
}

int32_t hid_get_field_value(const uint8_t *data, uint16_t data_len,
                             const hid_field_t *field,
                             uint8_t element_index)
{
    uint16_t bit_pos = field->bit_offset +
                       (uint16_t)element_index * field->bit_size;
    uint32_t value = hid_extract_bits(data, data_len, bit_pos, field->bit_size);

    /* Sign-extend if logical_min is negative */
    if (field->logical_min < 0 && field->bit_size < 32 &&
        (value & (1u << (field->bit_size - 1)))) {
        value |= ~((1u << field->bit_size) - 1);
    }
    return (int32_t)value;
}

/* ── public: debug dump ───────────────────────────────────────────── */

void hid_dump_field_map(const hid_field_map_t *map)
{
    for (int r = 0; r < map->num_reports; r++) {
        const hid_report_desc_t *rd = &map->reports[r];
        const char *ts = rd->type == 1 ? "INPUT" :
                         rd->type == 2 ? "OUTPUT" : "FEATURE";
        ESP_LOGI(TAG, "Report ID=%d %s  bits=%d  fields=%d",
                 rd->report_id, ts, rd->total_bits, rd->num_fields);
        for (int f = 0; f < rd->num_fields; f++) {
            const hid_field_t *fl = &rd->fields[f];
            ESP_LOGI(TAG, "  [%d] page=0x%04x usage=0x%04x..0x%04x "
                     "log=%d..%d  off=%d sz=%d cnt=%d flg=0x%02x",
                     f, fl->usage_page, fl->usage, fl->usage_max,
                     (int)fl->logical_min, (int)fl->logical_max,
                     fl->bit_offset, fl->bit_size, fl->count, fl->flags);
        }
    }
}
