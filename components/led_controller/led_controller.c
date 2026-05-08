#include "led_controller.h"
#include "event_bus.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "led_controller";

static led_strip_handle_t s_strip = NULL;
static uint8_t s_r = 0, s_g = 0, s_b = 0;
static bool s_on = false;

static void on_led_event(const char *event, const char *json, void *ctx) {
    if (strcmp(event, "led_set") != 0) return;

    int r = 0, g = 0, b = 0;
    if (sscanf(json, "{\"r\":%d,\"g\":%d,\"b\":%d}", &r, &g, &b) == 3) {
        led_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
    } else if (strstr(json, "\"on\":false")) {
        led_set_off();
    }
}

esp_err_t led_controller_init(void) {
    led_strip_config_t cfg = {
        .strip_gpio_num           = CONFIG_LED_CONTROLLER_GPIO,
        .max_leds                 = 1,
        .led_model                = LED_MODEL_WS2812,
        .color_component_format   = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out         = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t ret = led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(ret));
        return ret;
    }
    led_strip_clear(s_strip);
    return event_bus_subscribe(on_led_event, NULL);
}

esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    s_r = r; s_g = g; s_b = b; s_on = true;
    esp_err_t ret = led_strip_set_pixel(s_strip, 0, r, g, b);
    if (ret == ESP_OK) ret = led_strip_refresh(s_strip);

    char json[64];
    snprintf(json, sizeof(json), "{\"r\":%d,\"g\":%d,\"b\":%d,\"on\":true}", r, g, b);
    event_bus_post("led_state_changed", json);
    return ret;
}

esp_err_t led_set_off(void) {
    s_on = false;
    esp_err_t ret = led_strip_clear(s_strip);
    event_bus_post("led_state_changed", "{\"on\":false}");
    return ret;
}

esp_err_t led_get_state(uint8_t *r, uint8_t *g, uint8_t *b, bool *on) {
    *r = s_r; *g = s_g; *b = s_b; *on = s_on;
    return ESP_OK;
}
