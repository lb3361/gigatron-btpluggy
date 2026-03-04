#pragma once

#include "nvs.h"

#define NVS_NAMESPACE "btpluggy"

esp_err_t nvs_save(const char *key, const uint8_t *bda, const void *data, size_t len);
esp_err_t nvs_load(const char *key, const uint8_t *bda, void *data, size_t len);
esp_err_t nvs_clear_all(void);
