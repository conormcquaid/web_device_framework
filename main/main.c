#include "config_manager.h"
#include "event_bus.h"
#include "led_controller.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "main";

static void on_wifi_state_change(wifi_state_t state) {
    const char *names[] = {"DISCONNECTED", "CONNECTING", "CONNECTED", "AP_MODE"};
    ESP_LOGI(TAG, "WiFi state: %s", names[state]);
}

static void mdns_setup(void) {
    char device_name[64];
    config_get_str("device_name", device_name, sizeof(device_name));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(device_name));
    ESP_ERROR_CHECK(mdns_instance_name_set(device_name));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", device_name);
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(config_manager_init());
    mdns_setup();
    ESP_ERROR_CHECK(event_bus_init());
    ESP_ERROR_CHECK(led_controller_init());
    ESP_ERROR_CHECK(web_server_init());

    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_state_change));
    ESP_ERROR_CHECK(wifi_manager_start());
}
