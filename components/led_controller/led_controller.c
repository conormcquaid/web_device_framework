#include "led_controller.h"
#include "config_manager.h"
#include "event_bus.h"
#include "led_strip.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "led_controller";

static led_strip_handle_t s_strip      = NULL;
static uint32_t           s_led_count  = 1;
static bool               s_is_rgbw   = false;
static uint8_t            s_r = 0, s_g = 0, s_b = 0, s_w = 0;
static uint8_t            s_brightness = 255;
static bool               s_on        = false;
static uint8_t            s_gamma_lut[256];

static void build_gamma_lut(void) {
    for (int i = 0; i < 256; i++) {
        s_gamma_lut[i] = (uint8_t)(powf(i / 255.0f, 2.8f) * 255.0f + 0.5f);
    }
}

static led_color_component_format_t proto_to_fmt(const char *proto) {
    if (strcmp(proto, "RGB")  == 0) return LED_STRIP_COLOR_COMPONENT_FMT_RGB;
    if (strcmp(proto, "GRBW") == 0) return LED_STRIP_COLOR_COMPONENT_FMT_GRBW;
    if (strcmp(proto, "RGBW") == 0) return LED_STRIP_COLOR_COMPONENT_FMT_RGBW;
    return LED_STRIP_COLOR_COMPONENT_FMT_GRB;
}

static uint8_t scale(uint8_t v) {
    return (uint8_t)((s_gamma_lut[v] * (uint16_t)s_brightness) / 255);
}

static esp_err_t apply_pixels(void) {
    for (uint32_t i = 0; i < s_led_count; i++) {
        if (s_is_rgbw) {
            led_strip_set_pixel_rgbw(s_strip, i, scale(s_r), scale(s_g), scale(s_b), scale(s_w));
        } else {
            led_strip_set_pixel(s_strip, i, scale(s_r), scale(s_g), scale(s_b));
        }
    }
    return led_strip_refresh(s_strip);
}

static esp_err_t create_strip(void) {
    uint32_t gpio = 48, count = 1, brightness = 255;
    char proto[8] = "GRB";
    config_get_u32("led_gpio",       &gpio);
    config_get_u32("led_count",      &count);
    config_get_str("led_proto",      proto, sizeof(proto));
    config_get_u32("led_brightness", &brightness);
    if (count == 0 || count > 512) count = 1;
    if (brightness > 255) brightness = 255;
    s_led_count  = count;
    s_is_rgbw    = (strchr(proto, 'W') != NULL);
    s_brightness = (uint8_t)brightness;

    led_model_t model = s_is_rgbw ? LED_MODEL_SK6812 : LED_MODEL_WS2812;

    led_strip_config_t cfg = {
        .strip_gpio_num         = (int)gpio,
        .max_leds               = count,
        .led_model              = model,
        .color_component_format = proto_to_fmt(proto),
        .flags.invert_out       = false,
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
    ESP_LOGI(TAG, "LED strip: gpio=%lu count=%lu proto=%s brightness=%u",
             gpio, count, proto, s_brightness);
    return led_strip_clear(s_strip);
}

static void on_led_event(const char *event, const char *json, void *ctx) {
    if (strcmp(event, "led_set") != 0) return;
    int r = 0, g = 0, b = 0, w = 0;
    if (sscanf(json, "{\"r\":%d,\"g\":%d,\"b\":%d,\"w\":%d}", &r, &g, &b, &w) >= 3) {
        led_set_rgbw((uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)w);
    } else if (strstr(json, "\"on\":false")) {
        led_set_off();
    }
}

esp_err_t led_controller_init(void) {
    build_gamma_lut();
    esp_err_t ret = create_strip();
    if (ret != ESP_OK) return ret;
    return event_bus_subscribe(on_led_event, NULL);
}

esp_err_t led_controller_reinit(void) {
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    s_on = false;
    s_r = s_g = s_b = s_w = 0;
    return create_strip();
}

esp_err_t led_set_rgbw(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    s_r = r; s_g = g; s_b = b; s_w = w; s_on = true;
    esp_err_t ret = apply_pixels();

    char json[96];
    snprintf(json, sizeof(json),
             "{\"r\":%d,\"g\":%d,\"b\":%d,\"w\":%d,\"brightness\":%d,\"on\":true}",
             r, g, b, w, s_brightness);
    event_bus_post("led_state_changed", json);
    return ret;
}

esp_err_t led_set_brightness(uint8_t brightness) {
    s_brightness = brightness;
    config_set_u32("led_brightness", brightness);
    if (!s_on) return ESP_OK;
    esp_err_t ret = apply_pixels();

    char json[96];
    snprintf(json, sizeof(json),
             "{\"r\":%d,\"g\":%d,\"b\":%d,\"w\":%d,\"brightness\":%d,\"on\":true}",
             s_r, s_g, s_b, s_w, brightness);
    event_bus_post("led_state_changed", json);
    return ret;
}

esp_err_t led_set_off(void) {
    s_on = false;
    esp_err_t ret = led_strip_clear(s_strip);
    event_bus_post("led_state_changed", "{\"on\":false}");
    return ret;
}

esp_err_t led_get_state(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *w,
                         uint8_t *brightness, bool *on) {
    *r = s_r; *g = s_g; *b = s_b; *w = s_w;
    *brightness = s_brightness;
    *on = s_on;
    return ESP_OK;
}
