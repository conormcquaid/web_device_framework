#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WIFI_MAX_CREDS 8

typedef struct {
    char ssid[64];
    char pwd[32];
} wifi_cred_t;

esp_err_t config_manager_init(void);

esp_err_t config_get_str(const char *key, char *out, size_t max_len);
esp_err_t config_set_str(const char *key, const char *value);
esp_err_t config_get_u32(const char *key, uint32_t *out);
esp_err_t config_set_u32(const char *key, uint32_t value);
esp_err_t config_get_blob(const char *key, void *out, size_t *len);
esp_err_t config_set_blob(const char *key, const void *data, size_t len);

esp_err_t config_get_wifi_creds(wifi_cred_t *out, uint8_t *count);
esp_err_t config_set_wifi_creds(const wifi_cred_t *creds, uint8_t count);
esp_err_t config_add_wifi_cred(const char *ssid, const char *pwd);
esp_err_t config_remove_wifi_cred(const char *ssid);
