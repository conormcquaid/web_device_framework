#include "event_bus.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "event_bus";

ESP_EVENT_DEFINE_BASE(EVENT_BUS_BASE);

typedef struct {
    char event[64];
    char json_data[512];
} event_bus_msg_t;

typedef struct {
    event_bus_handler_t handler;
    void *ctx;
} subscriber_t;

#define MAX_SUBSCRIBERS 8

static esp_event_loop_handle_t s_loop;
static subscriber_t s_subscribers[MAX_SUBSCRIBERS];
static int s_sub_count = 0;

static void internal_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    event_bus_msg_t *msg = (event_bus_msg_t *)event_data;
    for (int i = 0; i < s_sub_count; i++) {
        s_subscribers[i].handler(msg->event, msg->json_data, s_subscribers[i].ctx);
    }
}

esp_err_t event_bus_init(void) {
    esp_event_loop_args_t args = {
        .queue_size      = 16,
        .task_name       = "event_bus",
        .task_stack_size = 4096,
        .task_priority   = 5,
        .task_core_id    = tskNO_AFFINITY,
    };
    esp_err_t ret = esp_event_loop_create(&args, &s_loop);
    if (ret != ESP_OK) return ret;

    return esp_event_handler_register_with(s_loop, EVENT_BUS_BASE,
                                           ESP_EVENT_ANY_ID, internal_handler, NULL);
}

esp_err_t event_bus_subscribe(event_bus_handler_t handler, void *ctx) {
    if (s_sub_count >= MAX_SUBSCRIBERS) return ESP_ERR_NO_MEM;
    s_subscribers[s_sub_count].handler = handler;
    s_subscribers[s_sub_count].ctx     = ctx;
    s_sub_count++;
    return ESP_OK;
}

esp_err_t event_bus_post(const char *event, const char *json_data) {
    event_bus_msg_t msg = {0};
    strlcpy(msg.event,     event,                sizeof(msg.event));
    strlcpy(msg.json_data, json_data ? json_data : "null", sizeof(msg.json_data));
    esp_err_t ret = esp_event_post_to(s_loop, EVENT_BUS_BASE, 0,
                                      &msg, sizeof(msg), pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post failed for event '%s': %s", event, esp_err_to_name(ret));
    }
    return ret;
}
