#include "hid.h"
#include "gap.h"
#include "hid_parser.h"
#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include <string.h>

#if CONFIG_BT_BLE_ENABLED
#include "esp_hidh_gattc.h"
#endif

static const char *TAG = "HID";

static hid_report_cb_t s_report_cb;
static hid_connection_cb_t s_conn_cb;

/* decoder callbacks */
static kb_event_cb_t s_kb_cb;
static void         *s_kb_user_data;
static gp_event_cb_t s_gp_cb;
static void         *s_gp_user_data;

#define MAX_CONNECTED 8

typedef enum {
    DEV_TYPE_UNKNOWN,
    DEV_TYPE_KEYBOARD,
    DEV_TYPE_GAMEPAD,
} dev_type_t;

typedef struct {
    uint8_t bda[6];
    bool    in_use;
    dev_type_t type;
    union {
        keyboard_t *keyboard;
        gamepad_t  *gamepad;
    } decoder;
    hid_field_map_t field_map;
} connected_dev_t;

static connected_dev_t s_connected[MAX_CONNECTED];
static int s_connected_count;

static connected_dev_t *find_device(const uint8_t *bda)
{
    for (int i = 0; i < MAX_CONNECTED; i++) {
        if (s_connected[i].in_use &&
            memcmp(s_connected[i].bda, bda, 6) == 0)
            return &s_connected[i];
    }
    return NULL;
}

static connected_dev_t *track_connect(const uint8_t *bda)
{
    for (int i = 0; i < MAX_CONNECTED; i++) {
        if (!s_connected[i].in_use) {
            memset(&s_connected[i], 0, sizeof(connected_dev_t));
            memcpy(s_connected[i].bda, bda, 6);
            s_connected[i].in_use = true;
            s_connected_count++;
            return &s_connected[i];
        }
    }
    ESP_LOGW(TAG, "connected device table full");
    return NULL;
}

static void track_disconnect(const uint8_t *bda)
{
    connected_dev_t *cdev = find_device(bda);
    if (!cdev) return;

    switch (cdev->type) {
    case DEV_TYPE_KEYBOARD:
        keyboard_destroy(cdev->decoder.keyboard);
        break;
    case DEV_TYPE_GAMEPAD:
        gamepad_destroy(cdev->decoder.gamepad);
        break;
    default:
        break;
    }

    cdev->in_use = false;
    s_connected_count--;
}

static int setup_decoder(connected_dev_t *cdev, esp_hidh_dev_t *dev)
{
    size_t num_maps = 0;
    esp_hid_raw_report_map_t *maps = NULL;

    esp_err_t err = esp_hidh_dev_report_maps_get(dev, &num_maps, &maps);
    if (err != ESP_OK || num_maps == 0 || !maps) {
        ESP_LOGW(TAG, "No report maps available");
        return 0;
    }

    err = hid_parse_report_map(maps[0].data, maps[0].len, &cdev->field_map);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to parse report descriptor");
        return 0;
    }

    hid_dump_field_map(&cdev->field_map);

    /* Try keyboard first, then gamepad */
    keyboard_t *kb = keyboard_create(&cdev->field_map,
                                     dev,
                                     s_kb_cb, s_kb_user_data);
    if (kb) {
        cdev->type = DEV_TYPE_KEYBOARD;
        cdev->decoder.keyboard = kb;
        ESP_LOGI(TAG, "Created keyboard decoder");
        return 1;
    }

    gamepad_t *gp = gamepad_create(&cdev->field_map, dev,
                                   s_gp_cb, s_gp_user_data);
    if (gp) {
        cdev->type = DEV_TYPE_GAMEPAD;
        cdev->decoder.gamepad = gp;
        ESP_LOGI(TAG, "Created gamepad decoder");
        return 1;
    }
    return 0;
}

static void hidh_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (p->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(p->open.dev);
            if (bda) {
                ESP_LOGI(TAG, "CONNECTED: " ESP_BD_ADDR_STR " %s",
                         ESP_BD_ADDR_HEX(bda),
                         esp_hidh_dev_name_get(p->open.dev));
                esp_hidh_dev_dump(p->open.dev, stdout);
                gap_stop_pairing();
                connected_dev_t *cdev = track_connect(bda);
                if (cdev && setup_decoder(cdev, p->open.dev) && s_conn_cb)
                    s_conn_cb(p->open.dev, true);
                break;
            }
        }
        ESP_LOGE(TAG, "OPEN FAILED: status=%d", p->open.status);
        gap_stop_pairing();
        break;
    }

    case ESP_HIDH_INPUT_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(p->input.dev);
        ESP_LOGD(TAG, ESP_BD_ADDR_STR " INPUT %s MAP:%u ID:%u Len:%d",
                 ESP_BD_ADDR_HEX(bda),
                 esp_hid_usage_str(p->input.usage),
                 p->input.map_index,
                 p->input.report_id,
                 p->input.length);
        // ESP_LOG_BUFFER_HEX(TAG, p->input.data, p->input.length);

        connected_dev_t *cdev = find_device(bda);
        if (cdev) {
            switch (cdev->type) {
            case DEV_TYPE_KEYBOARD:
                keyboard_process_report(cdev->decoder.keyboard, p->input.dev,
                                         p->input.report_id,
                                         p->input.data, p->input.length);
                break;
            case DEV_TYPE_GAMEPAD:
                gamepad_process_report(cdev->decoder.gamepad, p->input.dev,
                                       p->input.report_id,
                                       p->input.data, p->input.length);
                break;
            default:
                break;
            }
        }

        if (s_report_cb) {
            hid_report_t report = {
                .dev = p->input.dev,
                .transport = esp_hidh_dev_transport_get(p->input.dev),
                .usage = p->input.usage,
                .map_index = p->input.map_index,
                .report_id = p->input.report_id,
                .data = p->input.data,
                .length = p->input.length,
            };
            s_report_cb(&report);
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(p->close.dev);
        if (bda) {
            ESP_LOGI(TAG, "DISCONNECTED: " ESP_BD_ADDR_STR " %s reason=%d",
                     ESP_BD_ADDR_HEX(bda),
                     esp_hidh_dev_name_get(p->close.dev),
                     p->close.reason);
            track_disconnect(bda);
            if (s_conn_cb)
                s_conn_cb(p->close.dev, false);
        }
        esp_hidh_dev_free(p->close.dev);
#if EXPERIMENTAL_REBOOT_WHEN_LAST_DEVICE_RECONNECTS
        /* reboot when last device disconnects (rejuvenate) */
        if (s_connected_count == 0) {
            ESP_LOGI(TAG, "Last device disconnected: reboot");
            esp_restart();
        }
#endif
        break;
    }

    case ESP_HIDH_BATTERY_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(p->battery.dev);
        ESP_LOGI(TAG, "BATTERY: " ESP_BD_ADDR_STR " %d%%",
                 ESP_BD_ADDR_HEX(bda), p->battery.level);
        break;
    }

    case ESP_HIDH_START_EVENT:
        ESP_LOGI(TAG, "HID host started, status=%d", p->start.status);
        break;

    default:
        ESP_LOGD(TAG, "HID event %d", event);
        break;
    }
}

esp_err_t hid_init(hid_report_cb_t report_cb, hid_connection_cb_t conn_cb)
{
    s_report_cb = report_cb;
    s_conn_cb = conn_cb;
    memset(s_connected, 0, sizeof(s_connected));
    s_connected_count = 0;

#if CONFIG_BT_BLE_ENABLED
    ESP_RETURN_ON_ERROR(
        esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler),
        TAG, "gattc register");
#endif

    esp_hidh_config_t config = {
        .callback = hidh_event_handler,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    return esp_hidh_init(&config);
}

void hid_set_keyboard_cb(kb_event_cb_t cb, void *user_data)
{
    s_kb_cb = cb;
    s_kb_user_data = user_data;
}

void hid_set_gamepad_cb(gp_event_cb_t cb, void *user_data)
{
    s_gp_cb = cb;
    s_gp_user_data = user_data;
}

int hid_get_connected_count(void)
{
    return s_connected_count;
}

bool hid_is_device_connected(const uint8_t *bda)
{
    return find_device(bda) != NULL;
}

/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
