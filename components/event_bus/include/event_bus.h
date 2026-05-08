#pragma once
#include "esp_err.h"

typedef void (*event_bus_handler_t)(const char *event, const char *json_data, void *ctx);

esp_err_t event_bus_init(void);
esp_err_t event_bus_subscribe(event_bus_handler_t handler, void *ctx);
esp_err_t event_bus_post(const char *event, const char *json_data);
