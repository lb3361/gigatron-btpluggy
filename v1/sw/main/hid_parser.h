#pragma once

#include "esp_err.h"
#include <stdint.h>

/* ── field descriptor ─────────────────────────────────────────────── */

typedef struct {
    uint16_t usage_page;
    uint16_t usage;           /* single usage, or usage_min for ranges */
    uint16_t usage_max;       /* 0 if single usage */
    int32_t  logical_min;
    int32_t  logical_max;
    uint16_t bit_offset;      /* within report data (after report ID byte) */
    uint16_t bit_size;        /* bits per element */
    uint16_t count;           /* number of elements (report_count) */
    uint8_t  flags;           /* INPUT/OUTPUT/FEATURE main-item flags */
} hid_field_t;

/* Flag bits (from the INPUT/OUTPUT/FEATURE main-item data byte) */
#define HID_FLAG_CONSTANT   (1 << 0)   /* 0 = Data,     1 = Constant */
#define HID_FLAG_VARIABLE   (1 << 1)   /* 0 = Array,    1 = Variable */
#define HID_FLAG_RELATIVE   (1 << 2)   /* 0 = Absolute, 1 = Relative */

/* ── report descriptor ────────────────────────────────────────────── */

#define HID_MAX_FIELDS   16
#define HID_MAX_REPORTS   8

typedef struct {
    uint8_t  report_id;
    uint8_t  type;            /* 1 = INPUT, 2 = OUTPUT, 3 = FEATURE */
    uint16_t total_bits;
    uint8_t  num_fields;
    hid_field_t fields[HID_MAX_FIELDS];
} hid_report_desc_t;

typedef struct {
    uint8_t num_reports;
    hid_report_desc_t reports[HID_MAX_REPORTS];
} hid_field_map_t;

/* ── API ──────────────────────────────────────────────────────────── */

/* Parse a raw HID report descriptor into a field map. */
esp_err_t hid_parse_report_map(const uint8_t *desc, size_t len,
                                hid_field_map_t *out);

/* Extract an unsigned bit-field value from report data.
 * bit_offset and bit_size define the location; element_index
 * selects an element within a multi-element field. */
uint32_t hid_extract_bits(const uint8_t *data, uint16_t data_len,
                           uint16_t bit_offset, uint8_t bit_size);

/* Extract a signed field value, using the field's logical_min to
 * decide whether sign-extension is needed. */
int32_t hid_get_field_value(const uint8_t *data, uint16_t data_len,
                             const hid_field_t *field,
                             uint8_t element_index);

/* Log the parsed field map (for debugging). */
void hid_dump_field_map(const hid_field_map_t *map);
