#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t led_controller_init(void);
esp_err_t led_controller_reinit(void);
esp_err_t led_set_rgbw(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
esp_err_t led_set_brightness(uint8_t brightness);
esp_err_t led_set_off(void);
esp_err_t led_get_state(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *w, uint8_t *brightness, bool *on);
