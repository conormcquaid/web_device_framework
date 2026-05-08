#pragma once
#include "esp_err.h"

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE,
} wifi_state_t;

typedef void (*wifi_state_cb_t)(wifi_state_t state);

esp_err_t wifi_manager_init(wifi_state_cb_t callback);
esp_err_t wifi_manager_start(void);
wifi_state_t wifi_manager_get_state(void);
