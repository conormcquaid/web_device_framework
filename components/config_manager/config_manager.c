#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_manager";
static nvs_handle_t s_nvs;

static void set_default_str(const char *key, const char *val) {
    size_t len = 0;
    if (nvs_get_str(s_nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_str(s_nvs, key, val);
    }
}

static void set_default_u32(const char *key, uint32_t val) {
    uint32_t tmp;
    if (nvs_get_u32(s_nvs, key, &tmp) == ESP_ERR_NVS_NOT_FOUND) {
        nvs_set_u32(s_nvs, key, val);
    }
}

esp_err_t config_manager_init(void) {
    esp_err_t ret = nvs_open("cfg", NVS_READWRITE, &s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    set_default_str("device_name", "esp32-device");
    set_default_str("http_user",   "admin");
    set_default_str("http_pass",   "admin");
    set_default_u32("led_gpio",       48);
    set_default_u32("led_count",      1);
    set_default_str("led_proto",      "GRB");
    set_default_u32("led_brightness", 255);
    nvs_commit(s_nvs);
    return ESP_OK;
}

esp_err_t config_get_str(const char *key, char *out, size_t max_len) {
    return nvs_get_str(s_nvs, key, out, &max_len);
}

esp_err_t config_set_str(const char *key, const char *value) {
    esp_err_t ret = nvs_set_str(s_nvs, key, value);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

esp_err_t config_get_u32(const char *key, uint32_t *out) {
    return nvs_get_u32(s_nvs, key, out);
}

esp_err_t config_set_u32(const char *key, uint32_t value) {
    esp_err_t ret = nvs_set_u32(s_nvs, key, value);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

esp_err_t config_get_blob(const char *key, void *out, size_t *len) {
    return nvs_get_blob(s_nvs, key, out, len);
}

esp_err_t config_set_blob(const char *key, const void *data, size_t len) {
    esp_err_t ret = nvs_set_blob(s_nvs, key, data, len);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

esp_err_t config_get_wifi_creds(wifi_cred_t *out, uint8_t *count) {
    uint32_t n = 0;
    nvs_get_u32(s_nvs, "wifi_cred_n", &n);
    *count = (uint8_t)n;
    if (n == 0) return ESP_OK;
    size_t len = sizeof(wifi_cred_t) * n;
    return nvs_get_blob(s_nvs, "wifi_creds", out, &len);
}

esp_err_t config_set_wifi_creds(const wifi_cred_t *creds, uint8_t count) {
    esp_err_t ret = nvs_set_blob(s_nvs, "wifi_creds", creds, sizeof(wifi_cred_t) * count);
    if (ret == ESP_OK) ret = nvs_set_u32(s_nvs, "wifi_cred_n", count);
    if (ret == ESP_OK) ret = nvs_commit(s_nvs);
    return ret;
}

esp_err_t config_add_wifi_cred(const char *ssid, const char *pwd) {
    wifi_cred_t creds[WIFI_MAX_CREDS];
    uint8_t count = 0;
    config_get_wifi_creds(creds, &count);

    for (int i = 0; i < count; i++) {
        if (strcmp(creds[i].ssid, ssid) == 0) {
            strlcpy(creds[i].pwd, pwd, sizeof(creds[i].pwd));
            return config_set_wifi_creds(creds, count);
        }
    }
    if (count >= WIFI_MAX_CREDS) return ESP_ERR_NO_MEM;
    strlcpy(creds[count].ssid, ssid, sizeof(creds[count].ssid));
    strlcpy(creds[count].pwd,  pwd,  sizeof(creds[count].pwd));
    return config_set_wifi_creds(creds, count + 1);
}

esp_err_t config_remove_wifi_cred(const char *ssid) {
    wifi_cred_t creds[WIFI_MAX_CREDS];
    uint8_t count = 0;
    config_get_wifi_creds(creds, &count);

    wifi_cred_t kept[WIFI_MAX_CREDS];
    int kept_n = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(creds[i].ssid, ssid) != 0) {
            kept[kept_n++] = creds[i];
        }
    }
    return config_set_wifi_creds(kept, (uint8_t)kept_n);
}
