
#include "save.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <stdlib.h>
#include <string.h>

#define TAG "save"

static void bda_to_key(const char *key, const uint8_t *bda, char *fullkey)
{
    snprintf(fullkey, 32, "%s_%02X%02X%02X%02X%02X%02X", key,
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

esp_err_t nvs_save(const char *key, const uint8_t *bda, const void *data, size_t len)
{
    if (! key || !bda || !data)
        return ESP_ERR_INVALID_ARG;
    char fullkey[32];
    bda_to_key(key, bda, fullkey);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(h, fullkey, data, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Saved %d bytes of nvs data for %s", len, fullkey);
    else
        ESP_LOGE(TAG, "Failed to save %d bytes of nvs data for %s", len, fullkey);
    return err;
}

esp_err_t nvs_load(const char *key, const uint8_t *bda, void *data, size_t len)
{
    if (! key || !bda || !data)
        return ESP_ERR_INVALID_ARG;
    char fullkey[32];
    bda_to_key(key, bda, fullkey);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    size_t xlen = len;
    err = nvs_get_blob(h, fullkey, data, &xlen);
    if (xlen != len)
        err = ESP_FAIL;
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Loaded %d bytes of nvs data for %s", len, fullkey);
    nvs_close(h);
    return err;
}

esp_err_t nvs_clear_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Cleared all btpluggy nvs data");
    return err;
}

/* Local Variables: */
/* mode: c */
/* c-basic-offset: 4 */
/* indent-tabs-mode: () */
/* End: */
