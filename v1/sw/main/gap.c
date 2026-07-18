#include "gap.h"
#include "hid.h"
#include "save.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "GAP";

/* ── state ─────────────────────────────────────────────────────────── */

static volatile enum gap_state_t {
    S_IDLE,                     /* accepting connections from bonded bt device */
    S_RECONNECTING,             /* reconnecting already bonded ble device */
    S_PAIRING,                  /* scanning with the intention to pair */
    S_CONNECTING                /* connecting with the intention to pair */
} s_state;

static gap_event_cb_t s_event_cb;
static esp_timer_handle_t s_pairing_timer;
static TaskHandle_t s_scan_task_handle;
static int s_pause_ble_scans;

/* ── scan results ──────────────────────────────────────────────────── */

#define MAX_SCAN_RESULTS 16

typedef struct {
    uint8_t bda[6];
    esp_hid_transport_t transport;
    uint8_t ble_addr_type;
    char name[64];
    int rssi;
    bool in_use;
} scan_result_t;

static scan_result_t s_scan_results[MAX_SCAN_RESULTS];
static int s_scan_result_count;

/* ── synchronization ───────────────────────────────────────────────── */

static SemaphoreHandle_t s_bt_scan_sem;   /* BT Classic scan done */
static SemaphoreHandle_t s_ble_scan_sem;  /* BLE scan done */
static SemaphoreHandle_t s_ble_params_sem; /* BLE scan params set */

/* ── scan result helpers ───────────────────────────────────────────── */

static scan_result_t *find_scan_result(const uint8_t *bda)
{
    for (int i = 0; i < MAX_SCAN_RESULTS; i++) {
        if (s_scan_results[i].in_use &&
            memcmp(s_scan_results[i].bda, bda, 6) == 0)
            return &s_scan_results[i];
    }
    return NULL;
}

static scan_result_t *add_scan_result(const uint8_t *bda,
                                       esp_hid_transport_t transport,
                                       uint8_t ble_addr_type,
                                       const char *name, int rssi)
{
    if (find_scan_result(bda)) return NULL; /* already have it */

    for (int i = 0; i < MAX_SCAN_RESULTS; i++) {
        if (!s_scan_results[i].in_use) {
            scan_result_t *r = &s_scan_results[i];
            memcpy(r->bda, bda, 6);
            r->transport = transport;
            r->ble_addr_type = ble_addr_type;
            r->rssi = rssi;
            if (name) {
                strncpy(r->name, name, sizeof(r->name) - 1);
                r->name[sizeof(r->name) - 1] = '\0';
            } else {
                r->name[0] = '\0';
            }
            r->in_use = true;
            s_scan_result_count++;
            return r;
        }
    }
    return NULL;
}

static void clear_scan_results(void)
{
    memset(s_scan_results, 0, sizeof(s_scan_results));
    s_scan_result_count = 0;
}

/* ── bond check helpers ────────────────────────────────────────────── */

#if CONFIG_BT_HID_HOST_ENABLED
static bool is_bt_bonded(const uint8_t *bda)
{
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) return false;
    esp_bd_addr_t *list = malloc(count * sizeof(esp_bd_addr_t));
    if (!list) return false;
    esp_bt_gap_get_bond_device_list(&count, list);
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (memcmp(bda, list[i], 6) == 0) { found = true; break; }
    }
    free(list);
    return found;
}
#endif

#if CONFIG_BT_BLE_ENABLED
static bool is_ble_bonded(const uint8_t *bda)
{
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return false;
    esp_ble_bond_dev_t *list = malloc(count * sizeof(esp_ble_bond_dev_t));
    if (!list) return false;
    esp_ble_get_bond_device_list(&count, list);
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (memcmp(bda, list[i].bd_addr, 6) == 0) { found = true; break; }
    }
    free(list);
    return found;
}

static int unconnected_ble_bonded_count(void)
{
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return 0;
    esp_ble_bond_dev_t *list = malloc(count * sizeof(esp_ble_bond_dev_t));
    if (!list) return 0;
    esp_ble_get_bond_device_list(&count, list);
    int found = 0;
    for (int i = 0; i < count; i++)
        if (!hid_is_device_connected(list[i].bd_addr))
            found += 1;
    free(list);
    return found;
}
#endif

/* ── BT Classic GAP callback ──────────────────────────────────────── */

#if CONFIG_BT_HID_HOST_ENABLED

/* Check if CoD indicates a keyboard or gamepad (not mouse) */
static bool cod_is_acceptable(const esp_bt_cod_t *cod)
{
    if (cod->major != ESP_BT_COD_MAJOR_DEV_PERIPHERAL) return false;
    uint8_t minor = cod->minor;
    /* Bits 6-7 of minor: 01=keyboard, 10=mouse, 11=combo */
    bool is_keyboard = (minor & 0x10);
    bool is_mouse    = (minor & 0x20);
    if (is_mouse && !is_keyboard) return false;  /* pure mouse → reject */
    /* Bits 2-5: 01=joystick, 02=gamepad, etc.  Accept all except pure mouse */
    return true;
}

static void bt_gap_event_handler(esp_bt_gap_cb_event_t event,
                                  esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (s_state != S_PAIRING) break;

        struct disc_res_param *res = &param->disc_res;
        uint32_t codv = 0;
        esp_bt_cod_t *cod = (esp_bt_cod_t *)&codv;
        uint8_t *name = NULL;
        uint8_t name_len = 0;
        int8_t rssi = 0;

        for (int i = 0; i < res->num_prop; i++) {
            esp_bt_gap_dev_prop_t *prop = &res->prop[i];
            if (prop->type == ESP_BT_GAP_DEV_PROP_COD) {
                memcpy(&codv, prop->val, sizeof(uint32_t));
            } else if (prop->type == ESP_BT_GAP_DEV_PROP_RSSI) {
                rssi = *(int8_t *)prop->val;
            } else if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                name = (uint8_t *)prop->val;
                name_len = strlen((const char *)name);
            } else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
                /* Try to find name in EIR if not already found */
                if (!name) {
                    uint8_t len = 0;
                    uint8_t *data = esp_bt_gap_resolve_eir_data(
                        (uint8_t *)prop->val,
                        ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                    if (!data) {
                        data = esp_bt_gap_resolve_eir_data(
                            (uint8_t *)prop->val,
                            ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                    }
                    if (data && len) {
                        name = data;
                        name_len = len;
                    }
                }
            }
        }

        if (!cod_is_acceptable(cod)) {
            ESP_LOGD(TAG, "BT device rejected (CoD): " ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(res->bda));
            break;
        }

        /* Build a temporary name string */
        char name_str[64] = {0};
        if (name && name_len) {
            int copy_len = name_len < sizeof(name_str) - 1 ? name_len : sizeof(name_str) - 1;
            memcpy(name_str, name, copy_len);
        }

        ESP_LOGI(TAG, "BT HID found: " ESP_BD_ADDR_STR " %s rssi=%d",
                 ESP_BD_ADDR_HEX(res->bda), name_str, rssi);

        add_scan_result(res->bda, ESP_HID_TRANSPORT_BT, 0, name_str, rssi);
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            xSemaphoreGive(s_bt_scan_sem);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT: {
        ESP_LOGI(TAG, "BT SSP confirm from " ESP_BD_ADDR_STR,
                 ESP_BD_ADDR_HEX(param->cfm_req.bda));
        bool accept = gap_is_pairing_active() || is_bt_bonded(param->cfm_req.bda);
        if (!accept) {
            ESP_LOGW(TAG, "Rejecting SSP (not pairing, not bonded)");
        }
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, accept);
        break;
    }

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "BT passkey: %06"PRIu32, param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "BT key request");
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(TAG, "BT SSP pin req from " ESP_BD_ADDR_STR,
                 ESP_BD_ADDR_HEX(param->cfm_req.bda));
        bool accept = gap_is_pairing_active() || is_bt_bonded(param->pin_req.bda);
        if (accept) {
            esp_bt_pin_code_t pin = {0};
            esp_bt_gap_pin_reply(param->pin_req.bda, true,
                                 param->pin_req.min_16_digit ? 16 : 4, pin);
        } else {
            ESP_LOGW(TAG, "Rejecting PIN (not pairing, not bonded)");
            esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, NULL);
        }
        break;
    }

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "BT auth success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "BT auth failed: 0x%x", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGD(TAG, "BT mode change: %d", param->mode_chg.mode);
        break;

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        if (param->acl_conn_cmpl_stat.stat == ESP_OK
            && is_bt_bonded(param->acl_conn_cmpl_stat.bda)
            && s_state == S_IDLE ) {
            ESP_LOGI(TAG, "BT reconnection " ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(param->acl_conn_cmpl_stat.bda));
            /* pause ble activity to possibly help */
            s_pause_ble_scans = 20;
        }
        break;

    default:
        ESP_LOGD(TAG, "BT GAP event %d", event);
        break;
    }
}

#endif /* CONFIG_BT_HID_HOST_ENABLED */

/* ── BLE GAP callback ─────────────────────────────────────────────── */

#if CONFIG_BT_BLE_ENABLED

static void ble_gap_event_handler(esp_gap_ble_cb_event_t event,
                                   esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        xSemaphoreGive(s_ble_params_sem);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        struct ble_scan_result_evt_param *sr = &param->scan_rst;
        switch (sr->search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {

            char name_str[64] = {0};

            if (is_ble_bonded(sr->bda) && !hid_is_device_connected(sr->bda)) {

                ESP_LOGI(TAG, "Bonded BLE HID found: " ESP_BD_ADDR_STR " %s",
                         ESP_BD_ADDR_HEX(sr->bda), name_str);
                add_scan_result(sr->bda, ESP_HID_TRANSPORT_BLE,
                                sr->ble_addr_type, name_str, sr->rssi);

            } else if (s_state == S_PAIRING) {

                /* Extract UUID */
                uint8_t uuid_len = 0;
                uint8_t *uuid_d = esp_ble_resolve_adv_data_by_type(
                    sr->ble_adv,
                    sr->adv_data_len + sr->scan_rsp_len,
                    ESP_BLE_AD_TYPE_16SRV_CMPL, &uuid_len);
                if (!uuid_d || uuid_len < 2) break;
                uint16_t uuid = uuid_d[0] | (uuid_d[1] << 8);
                if (uuid != 0x1812) break;  /* not HID service */

                /* Extract appearance */
                uint8_t app_len = 0;
                uint8_t *app_d = esp_ble_resolve_adv_data_by_type(
                    sr->ble_adv,
                    sr->adv_data_len + sr->scan_rsp_len,
                    ESP_BLE_AD_TYPE_APPEARANCE, &app_len);
                uint16_t appearance = 0;
                if (app_d && app_len >= 2) {
                    appearance = app_d[0] | (app_d[1] << 8);
                }
                if (appearance == ESP_HID_APPEARANCE_MOUSE) break;

                /* Extract name */
                uint8_t name_len = 0;
                uint8_t *name_d = esp_ble_resolve_adv_data_by_type(
                    sr->ble_adv,
                    sr->adv_data_len + sr->scan_rsp_len,
                    ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
                if (!name_d) {
                    name_d = esp_ble_resolve_adv_data_by_type(
                        sr->ble_adv,
                        sr->adv_data_len + sr->scan_rsp_len,
                        ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
                }
                if (name_d && name_len) {
                    int copy = name_len < sizeof(name_str) - 1 ? name_len : sizeof(name_str) - 1;
                    memcpy(name_str, name_d, copy);
                }
                ESP_LOGI(TAG, "BLE HID found: " ESP_BD_ADDR_STR " %s rssi=%d app=0x%04x",
                         ESP_BD_ADDR_HEX(sr->bda), name_str, sr->rssi, appearance);
                add_scan_result(sr->bda, ESP_HID_TRANSPORT_BLE,
                                sr->ble_addr_type, name_str, sr->rssi);
            }
            break;
        }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            xSemaphoreGive(s_ble_scan_sem);
            break;
        default:
            break;
        }
        break;
    }

    case ESP_GAP_BLE_SEC_REQ_EVT: {
        bool accept = gap_is_pairing_active() ||
                      is_ble_bonded(param->ble_security.ble_req.bd_addr);
        if (!accept) {
            ESP_LOGW(TAG, "Rejecting BLE security (not pairing, not bonded)");
        } else {
            ESP_LOGI(TAG, "Accepting BLE security request");
        }
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, accept);
        break;
    }

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "BLE auth success");
        } else {
            ESP_LOGE(TAG, "BLE auth failed: 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGD(TAG, "BLE key type=%d",
                 param->ble_security.ble_key.key_type);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGI(TAG, "BLE passkey req (just-works, sending 0)");
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, 0);
        break;

    default:
        ESP_LOGV(TAG, "BLE GAP event %d", event);
        break;
    }
}

#endif /* CONFIG_BT_BLE_ENABLED */

/* ── BLE scan parameters ───────────────────────────────────────────── */

#if CONFIG_BT_BLE_ENABLED
static esp_ble_scan_params_t s_ble_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_ENABLE,
};
#endif

/* ── scan task ─────────────────────────────────────────────────────── */


static void do_scan(uint32_t seconds, esp_bt_mode_t mode)
{
#if CONFIG_BT_BLE_ENABLED
    if (mode == ESP_BT_MODE_BTDM || mode == ESP_BT_MODE_BLE) {
        ESP_LOGI(TAG, "BLE scan %"PRIu32"s ...", seconds);
        esp_err_t ret = esp_ble_gap_set_scan_params(&s_ble_scan_params);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BLE scan params failed: %d", ret);
            return;
        }
        xSemaphoreTake(s_ble_params_sem, pdMS_TO_TICKS(2000));
        ret = esp_ble_gap_start_scanning(seconds);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BLE scan start failed: %d", ret);
            return;
        }
    }
#endif
#if CONFIG_BT_HID_HOST_ENABLED
    if (mode == ESP_BT_MODE_BTDM || mode == ESP_BT_MODE_CLASSIC_BT) {
        ESP_LOGI(TAG, "BT Classic scan %"PRIu32"s ...", seconds);
        esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                                   (int)(seconds / 1.28), 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BT scan start failed: %d", ret);
            return;
        }
    }
#endif     
#if CONFIG_BT_BLE_ENABLED
    if (mode == ESP_BT_MODE_BTDM || mode == ESP_BT_MODE_BLE)
        xSemaphoreTake(s_ble_scan_sem, pdMS_TO_TICKS(seconds * 1000 + 2000));
#endif
#if CONFIG_BT_HID_HOST_ENABLED
    if (mode == ESP_BT_MODE_BTDM || mode == ESP_BT_MODE_CLASSIC_BT) 
        xSemaphoreTake(s_bt_scan_sem, pdMS_TO_TICKS(seconds * 1000 + 2000));
#endif
    ESP_LOGI(TAG, "Scan done, found %d total", s_scan_result_count);
}

static void connect_scan_results(void)
{
    for (int i = 0; i < MAX_SCAN_RESULTS; i++) {
        if (!s_scan_results[i].in_use) continue;
        scan_result_t *r = &s_scan_results[i];

        /* Skip if already connected */
        if (hid_is_device_connected(r->bda)) {
            ESP_LOGD(TAG, "Already connected: " ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(r->bda));
            continue;
        }
        ESP_LOGI(TAG, "Connecting to %s " ESP_BD_ADDR_STR " ...",
                 r->transport == ESP_HID_TRANSPORT_BT ? "BT" : "BLE",
                 ESP_BD_ADDR_HEX(r->bda));

        /* Stop pairing */
        esp_timer_stop(s_pairing_timer);
#if CONFIG_BT_HID_HOST_ENABLED
        if (s_state == S_PAIRING)
            if (r->transport == ESP_HID_TRANSPORT_BT && is_bt_bonded(r->bda))
               esp_bt_gap_remove_bond_device(r->bda);
#endif
        /* Adjust state */
        s_state = (s_state == S_PAIRING) ? S_CONNECTING : S_RECONNECTING;
        
        /* Open hid device */
        /* For BLE, esp_hidh_dev_open blocks until done.
         * For BT Classic, it returns immediately and connection proceeds async. */
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_hidh_dev_t *dev = esp_hidh_dev_open(r->bda, r->transport,
                                                r->ble_addr_type);
        if (dev == NULL) {
            ESP_LOGE(TAG, "Failed to open device");
        } else {
            break; // Connect one by one.
        }
    }
}

static void scan_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_state == S_CONNECTING || s_state == S_RECONNECTING)
            continue;
        clear_scan_results();
        if (s_state == S_PAIRING)
            do_scan(6, ESP_BT_MODE_BTDM);
        else if (s_pause_ble_scans > 0)
            s_pause_ble_scans -= 1;
#if CONFIG_BT_BLE_ENABLED
        else if (unconnected_ble_bonded_count() > 0)
            do_scan(3, ESP_BT_MODE_BLE);
#endif
        if (s_scan_result_count > 0)
            connect_scan_results();
    }
}

/* ── timers ────────────────────────────────────────────────────────── */

static void pairing_timeout_cb(void *arg)
{
    ESP_LOGI(TAG, "Pairing timeout");
    gap_stop_pairing();
}

/* ── public API ────────────────────────────────────────────────────── */

esp_err_t gap_init(gap_event_cb_t callback)
{
    s_event_cb = callback;

    s_bt_scan_sem = xSemaphoreCreateBinary();
    s_ble_scan_sem = xSemaphoreCreateBinary();
    s_ble_params_sem = xSemaphoreCreateBinary();

#if CONFIG_BT_HID_HOST_ENABLED
    /* BT Classic GAP */
    ESP_RETURN_ON_ERROR(
        esp_bt_gap_register_callback(bt_gap_event_handler), TAG, "bt gap cb");

    /* SSP IO Capabilities.
     * Using CAP_IO causes  ESP_BT_GAP_CFM_REQ_EVT.
     * But is it still true if the other device says CAP_NONE? */
    esp_bt_sp_param_t sp_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(sp_type, &iocap, sizeof(iocap));

    /* Legacy PIN.
     * Using variable type causes ESP_BT_GAP_PIN_REQ_EVT. */
    esp_bt_pin_code_t pin = {0};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 4, pin);

    /* Connectable but not discoverable */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
#endif

#if CONFIG_BT_BLE_ENABLED
    /* BLE GAP */
    ESP_RETURN_ON_ERROR(
        esp_ble_gap_register_callback(ble_gap_event_handler), TAG, "ble gap cb");

    /* BLE security: just-works with bonding */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t ble_iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ble_iocap, sizeof(ble_iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
#endif

    /* Pairing timeout timer */
    esp_timer_create_args_t pt_args = {
        .callback = pairing_timeout_cb,
        .name = "pair_tmr",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&pt_args, &s_pairing_timer), TAG, "pair timer");

    /* Scan task — pinned to core 0 */
    xTaskCreatePinnedToCore(scan_task, "gap_scan", 6 * 1024, NULL, 5,
                            &s_scan_task_handle, 0);

    ESP_LOGI(TAG, "GAP initialized");
    return ESP_OK;
}

esp_err_t gap_start_pairing(uint32_t timeout_seconds)
{
    switch (s_state) {
    case S_PAIRING:
        /* Already pairing — reset timer */
        esp_timer_stop(s_pairing_timer);
        esp_timer_start_once(s_pairing_timer, timeout_seconds * 1000000ULL);
        ESP_LOGI(TAG, "Pairing timer reset to %"PRIu32"s", timeout_seconds);
        return ESP_OK;

    default: // S_{RE}CONNECTING
        /* Cannot do this now. Come back. */
        return ESP_FAIL;
        
    case S_IDLE:
        s_state = S_PAIRING;
        esp_timer_start_once(s_pairing_timer, timeout_seconds * 1000000ULL);
        ESP_LOGI(TAG, ">>> PAIRING MODE ACTIVE (%"PRIu32"s) <<<", timeout_seconds);
        if (s_event_cb) s_event_cb(GAP_EVT_PAIRING_START, NULL);
        /* Wake the scan task */
        xTaskNotifyGive(s_scan_task_handle);
        return ESP_OK;
    }
}

esp_err_t gap_stop_pairing(void)
{
    if (s_state == S_IDLE) {
        return ESP_OK;
    }
    if (s_state == S_PAIRING) {
        esp_timer_stop(s_pairing_timer);
#if CONFIG_BT_HID_HOST_ENABLED
        esp_bt_gap_cancel_discovery();
#endif
#if CONFIG_BT_BLE_ENABLED
        esp_ble_gap_stop_scanning();
#endif
        ESP_LOGI(TAG, ">>> PAIRING MODE ENDED <<<");
        if (s_event_cb)
            s_event_cb(GAP_EVT_PAIRING_END, NULL);
    }
    s_state = S_IDLE;
    return ESP_OK;
}

bool gap_is_pairing_active(void)
{
    return s_state >= S_PAIRING;
}

int gap_get_bonded_count(void)
{
    int count = 0;
#if CONFIG_BT_HID_HOST_ENABLED
    int bt = esp_bt_gap_get_bond_device_num();
    if (bt > 0) count += bt;
#endif
#if CONFIG_BT_BLE_ENABLED
    int ble = esp_ble_get_bond_device_num();
    if (ble > 0) count += ble;
#endif
    return count;
}

esp_err_t gap_clear_all_bonds(void)
{
    ESP_LOGW(TAG, "Clearing all bonds");
#if CONFIG_BT_HID_HOST_ENABLED
    {
        int count = esp_bt_gap_get_bond_device_num();
        if (count > 0) {
            esp_bd_addr_t *list = malloc(count * sizeof(esp_bd_addr_t));
            if (list) {
                esp_bt_gap_get_bond_device_list(&count, list);
                for (int i = 0; i < count; i++) {
                    esp_bt_gap_remove_bond_device(list[i]);
                }
                free(list);
            }
        }
    }
#endif
#if CONFIG_BT_BLE_ENABLED
    {
        int count = esp_ble_get_bond_device_num();
        if (count > 0) {
            esp_ble_bond_dev_t *list = malloc(count * sizeof(esp_ble_bond_dev_t));
            if (list) {
                esp_ble_get_bond_device_list(&count, list);
                for (int i = 0; i < count; i++) {
                    esp_ble_remove_bond_device(list[i].bd_addr);
                }
                free(list);
            }
        }
    }
#endif
    /* Also clear any saved gamepad profiles */
    nvs_clear_all();
    ESP_LOGI(TAG, "All bonds cleared");
    return ESP_OK;
}

/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
