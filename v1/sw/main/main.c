#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "driver/gpio.h"

#include "gap.h"
#include "hid.h"
#include "led.h"
#include "gigatron.h"
#include "pluggy.h"

static const char *TAG = "MAIN";

#define BUTTON_GPIO      GPIO_NUM_35
#define LONG_PRESS_MS    3000

static TaskHandle_t s_button_task_handle;

/* ── callbacks ─────────────────────────────────────────────────────── */

static void gap_callback(gap_event_t event, void *param)
{
    if (gap_is_pairing_active())
        led_set_persistent(0, 0, 32, 500);
    else if (hid_get_connected_count() > 0)
        led_set_persistent(0, 0, 32, 0);
    else
        led_set_persistent(0, 0, 0, 0);
    if  (event == GAP_EVT_DEVICE_CONNECTED)
        led_set_transient(0, 32, 0, 1000);
}

static void hid_conn_cb(esp_hidh_dev_t *dev, bool connected)
{
    gap_callback((connected) ? GAP_EVT_DEVICE_CONNECTED : GAP_EVT_DEVICE_DISCONNECTED, NULL);
}

static void hid_report_cb(const hid_report_t *report)
{
    (void)report;
}

static void kb_event_handler(const kb_event_t *ev, void *user_data)
{
    int c = ev->giga_key;
    if (c < ' ' || c >= 0x7f) { c = ' '; }
    
    ESP_LOGI(TAG, "KB: %s scancode=0x%02x mod=0x%02x g-btn=0x%02x g-key=0x%02x (%c)",
             ev->type == KB_KEY_DOWN ? "DOWN" : "UP",
             ev->scancode, ev->modifiers, ev->giga_buttons, ev->giga_key, c);
    
    /* Post events into Gigatron interface */
    gigatron_post((ev->type == KB_KEY_DOWN) ? ev->giga_key : 0xff, ev->giga_buttons);
}

static void gp_event_handler(const gp_state_t *st, void *user_data)
{
    ESP_LOGI(TAG, "GP: A=%d B=%d START=%d SEL=%d "
             "U=%d D=%d L=%d R=%d raw=0x%04x g-btn=0x%02x",
             st->buttons[GP_BTN_A], st->buttons[GP_BTN_B],
             st->buttons[GP_BTN_START], st->buttons[GP_BTN_SELECT],
             st->dpad_up, st->dpad_down, st->dpad_left, st->dpad_right,
             st->raw_buttons, st->giga_buttons);

    /* Post gamepad event into Gigatron interface */
    gigatron_post(0xFF, st->giga_buttons);
}


static void pluggy_rx_cb(int byte)
{
    ESP_LOGI(TAG, "RX callback received %d (%c)", byte,
             (byte >= 32 && byte < 127) ? byte : '?');
}


/* ── button handling ───────────────────────────────────────────────── */

static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_button_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_task(void *arg)
{
    // this task is pinned to core 0.
    // gpio interrupt handlers then run on core 0 as well
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL));

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50)); /* debounce */

        if (gpio_get_level(BUTTON_GPIO) != 0) continue;

        /* Measure hold duration */
        TickType_t start = xTaskGetTickCount();
        while (gpio_get_level(BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        uint32_t held_ms = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;

        if (held_ms >= LONG_PRESS_MS) {
            ESP_LOGW(TAG, "Long press — clearing all bonds");
            gap_stop_pairing();
            gap_clear_all_bonds();
            led_set_transient(64, 0, 0, 1000);
        } else {
            if (gap_is_pairing_active()) {
                ESP_LOGI(TAG, "Button press — stopping pairing");
                gap_stop_pairing();
            } else {
                ESP_LOGI(TAG, "Button press — starting pairing");
                gap_start_pairing(120);
            }
        }
    }
}

/* ── app_main ──────────────────────────────────────────────────────── */

#if CONFIG_BTDM_CTRL_MODE_BTDM
# define BT_CTRL_MODE ESP_BT_MODE_BTDM
#elif CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY
# define BT_CTRL_MODE ESP_BT_MODE_CLASSIC_BT
#else
# define BT_CTRL_MODE ESP_BT_MODE_BLE
#endif

void app_main(void)
{
    /* 0. Gigatron interface init */
    gigatron_init();

    /* 1. NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. LEDs */
    ESP_ERROR_CHECK(led_init());

    /* 3. BT controller — dual mode */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = BT_CTRL_MODE;
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(BT_CTRL_MODE));

    /* 4. Bluedroid */
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bd_cfg.ssp_en = true;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* 5. Device name */
#if CONFIG_BT_HID_HOST_ENABLED
    esp_bt_gap_set_device_name("BtPluggy");
#endif
#if CONFIG_BT_BLE_ENABLED
    esp_ble_gap_set_device_name("BtPluggy");
#endif
    
    /* 6. GAP layer */
    ESP_ERROR_CHECK(gap_init(gap_callback));

    /* 7. HID host layer */
    ESP_ERROR_CHECK(hid_init(hid_report_cb, hid_conn_cb));
    hid_set_keyboard_cb(kb_event_handler, NULL);
    hid_set_gamepad_cb(gp_event_handler, NULL);

    /* 8. Button on GPIO35 */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,  /* external pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    /* 9. Button task — pinned to core 0 */
    xTaskCreatePinnedToCore(button_task, "button", 3 * 1024, NULL, 10,
                            &s_button_task_handle, 0);

    /* 10. Ready */
    const uint8_t *bda = esp_bt_dev_get_address();
    if (bda) {
        ESP_LOGI(TAG, "BDA: %02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }
    ESP_LOGI(TAG, "Bonded devices: %d", gap_get_bonded_count());
    ESP_LOGI(TAG, "Ready. Short press GPIO35 to pair (120s). Long press to clear bonds.");

    /* 11. Gigatron RX */
    pluggy_init(pluggy_rx_cb); // disable for now.
}

/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
