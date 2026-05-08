#include "wifi_manager.h"
#include "web_server.h"
#include "event_bus.h"
#include "config_manager.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "wifi_manager";

typedef struct {
    wifi_cred_t cred;
    int8_t      rssi;
} matched_ap_t;

static wifi_state_t   s_state       = WIFI_STATE_DISCONNECTED;
static wifi_state_cb_t s_callback   = NULL;
static matched_ap_t   s_matched[WIFI_MAX_CREDS];
static int            s_match_count = 0;
static int            s_retry_idx   = 0;
static uint32_t       s_backoff_s   = 2;
static TimerHandle_t  s_rescan_timer = NULL;

static void set_state(wifi_state_t state) {
    s_state = state;
    if (s_callback) s_callback(state);
}

static void do_connect(int idx) {
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid,     s_matched[idx].cred.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_matched[idx].cred.pwd,  sizeof(cfg.sta.password));
    ESP_LOGI(TAG, "connecting to '%s'", s_matched[idx].cred.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());
    set_state(WIFI_STATE_CONNECTING);
}

static void rescan_cb(TimerHandle_t t) {
    ESP_LOGI(TAG, "starting WiFi scan");
    esp_wifi_scan_start(NULL, false);
}

static void schedule_rescan(uint32_t delay_s) {
    TickType_t ticks = pdMS_TO_TICKS(delay_s * 1000);
    if (s_rescan_timer == NULL) {
        s_rescan_timer = xTimerCreate("wifi_rescan", ticks, pdFALSE, NULL, rescan_cb);
        xTimerStart(s_rescan_timer, 0);
    } else {
        xTimerChangePeriod(s_rescan_timer, ticks, 0);
        xTimerStart(s_rescan_timer, 0);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_SCAN_DONE) {
        uint16_t ap_count = 20;
        wifi_ap_record_t ap_records[20];
        esp_wifi_scan_get_ap_records(&ap_count, ap_records);

        wifi_cred_t creds[WIFI_MAX_CREDS];
        uint8_t cred_count = 0;
        config_get_wifi_creds(creds, &cred_count);

        matched_ap_t matches[WIFI_MAX_CREDS];
        int mc = 0;
        for (int c = 0; c < cred_count; c++) {
            int8_t best = INT8_MIN;
            for (int a = 0; a < ap_count; a++) {
                if (strcmp((char *)ap_records[a].ssid, creds[c].ssid) == 0 &&
                    ap_records[a].rssi > best) {
                    best = ap_records[a].rssi;
                }
            }
            if (best != INT8_MIN) {
                matches[mc].cred = creds[c];
                matches[mc].rssi = best;
                mc++;
            }
        }

        /* insertion-sort by RSSI descending */
        for (int i = 1; i < mc; i++) {
            matched_ap_t tmp = matches[i];
            int j = i - 1;
            while (j >= 0 && matches[j].rssi < tmp.rssi) {
                matches[j + 1] = matches[j];
                j--;
            }
            matches[j + 1] = tmp;
        }

        s_match_count = mc;
        memcpy(s_matched, matches, sizeof(matched_ap_t) * mc);

        if (mc > 0) {
            s_retry_idx = 0;
            s_backoff_s = 2;
            do_connect(0);
        } else {
            ESP_LOGI(TAG, "no matching networks found, rescan in 30s");
            set_state(WIFI_STATE_AP_MODE);
            schedule_rescan(30);
        }

    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGI(TAG, "STA disconnected, reason %d", disc->reason);
        s_retry_idx++;
        if (s_retry_idx < s_match_count) {
            do_connect(s_retry_idx);
        } else {
            set_state(WIFI_STATE_AP_MODE);
            s_retry_idx = 0;
            ESP_LOGI(TAG, "all creds exhausted, rescan in %lus", (unsigned long)s_backoff_s);
            schedule_rescan(s_backoff_s);
            s_backoff_s = (s_backoff_s * 2 > 30) ? 30 : s_backoff_s * 2;
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_backoff_s = 2;
        set_state(WIFI_STATE_CONNECTED);
        web_server_start();
    }
}

static void on_reconnect_event(const char *event, const char *json, void *ctx) {
    if (strcmp(event, "wifi_reconnect") != 0) return;
    ESP_LOGI(TAG, "reconnect requested");
    esp_wifi_disconnect();
    esp_wifi_scan_start(NULL, false);
}

esp_err_t wifi_manager_init(wifi_state_cb_t callback) {
    s_callback = callback;

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) return ret;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_ip_event, NULL, NULL));
    return event_bus_subscribe(on_reconnect_event, NULL);
}

esp_err_t wifi_manager_start(void) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    char device_name[64];
    config_get_str("device_name", device_name, sizeof(device_name));

    char ap_ssid[72];
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02x%02x%02x",
             device_name, mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID '%s'", ap_ssid);
    set_state(WIFI_STATE_AP_MODE);
    web_server_start();

    wifi_cred_t creds[WIFI_MAX_CREDS];
    uint8_t count = 0;
    config_get_wifi_creds(creds, &count);
    if (count > 0) {
        esp_wifi_scan_start(NULL, false);
    }
    return ESP_OK;
}

wifi_state_t wifi_manager_get_state(void) {
    return s_state;
}
