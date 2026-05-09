#include "web_server.h"
#include "event_bus.h"
#include "config_manager.h"
#include "led_controller.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "web_server";

static httpd_handle_t s_server   = NULL;
static QueueHandle_t  s_ws_queue = NULL;

/* ── LittleFS ─────────────────────────────────────────────────────────── */

static esp_err_t mount_littlefs(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = "/littlefs",
        .partition_label        = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ── Basic Auth ───────────────────────────────────────────────────────── */

static bool check_auth(httpd_req_t *req) {
    char auth[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK)
        return false;
    if (strncmp(auth, "Basic ", 6) != 0)
        return false;

    unsigned char decoded[128];
    size_t dec_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &dec_len,
                               (unsigned char *)auth + 6, strlen(auth + 6)) != 0)
        return false;
    decoded[dec_len] = '\0';

    char http_user[64], http_pass[64];
    config_get_str("http_user", http_user, sizeof(http_user));
    config_get_str("http_pass", http_pass, sizeof(http_pass));

    char expected[130];
    snprintf(expected, sizeof(expected), "%s:%s", http_user, http_pass);
    return strcmp((char *)decoded, expected) == 0;
}

static esp_err_t send_401(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

#define AUTH_CHECK(req) \
    do { if (!check_auth(req)) return send_401(req); } while (0)

/* ── Helpers ──────────────────────────────────────────────────────────── */

static const char *mime_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    return "application/octet-stream";
}

static char *recv_body(httpd_req_t *req, int max) {
    if (req->content_len == 0 || req->content_len > (size_t)max) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) { free(buf); return NULL; }
    buf[received] = '\0';
    return buf;
}

/* ── Probe logger ─────────────────────────────────────────────────────── */

static const char *method_name(int m) {
    switch (m) {
        case HTTP_GET:     return "GET";
        case HTTP_POST:    return "POST";
        case HTTP_PUT:     return "PUT";
        case HTTP_DELETE:  return "DELETE";
        case HTTP_PATCH:   return "PATCH";
        case HTTP_HEAD:    return "HEAD";
        case HTTP_OPTIONS: return "OPTIONS";
        default:           return "UNKNOWN";
    }
}

static void log_probe(httpd_req_t *req) {
    char ip[48] = "?";
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr = {0};
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_INET) {
            inet_ntop(AF_INET,  &((struct sockaddr_in  *)&addr)->sin_addr,  ip, sizeof(ip));
        } else {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr, ip, sizeof(ip));
        }
    }

    char ua[128]  = ""; httpd_req_get_hdr_value_str(req, "User-Agent",   ua,   sizeof(ua));
    char host[64] = ""; httpd_req_get_hdr_value_str(req, "Host",         host, sizeof(host));
    char ct[64]   = ""; httpd_req_get_hdr_value_str(req, "Content-Type", ct,   sizeof(ct));

    ESP_LOGW(TAG, "PROBE %s %s | from=%s host=%s ua=%s ct=%s",
             method_name(req->method), req->uri, ip, host, ua, ct);

    /* Build WS envelope and drop directly onto the queue — do NOT call
       event_bus_post() here. Routing through the event bus eventually calls
       httpd_ws_send_frame_async → httpd_queue_work, which re-enters the httpd
       from within an httpd handler and can stall active connections. */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "method", method_name(req->method));
    cJSON_AddStringToObject(data, "uri",    req->uri);
    cJSON_AddStringToObject(data, "ip",     ip);
    cJSON_AddStringToObject(data, "host",   host);
    cJSON_AddStringToObject(data, "ua",     ua);
    if (ct[0]) cJSON_AddStringToObject(data, "ct", ct);
    char *data_str = cJSON_PrintUnformatted(data);
    cJSON_Delete(data);
    if (!data_str) return;

    size_t msg_len = strlen(data_str) + 32;
    char *msg = malloc(msg_len);
    if (msg) {
        snprintf(msg, msg_len, "{\"event\":\"probe\",\"data\":%s}", data_str);
        if (xQueueSend(s_ws_queue, &msg, 0) != pdTRUE) free(msg);
    }
    free(data_str);
}

static esp_err_t err_probe_handler(httpd_req_t *req, httpd_err_code_t err) {
    /* Send the response first, then log — socket remains valid for getpeername
       but the httpd is no longer mid-transaction when we touch the queue. */
    httpd_err_code_t resp_err = (err == HTTPD_405_METHOD_NOT_ALLOWED)
                                ? HTTPD_405_METHOD_NOT_ALLOWED
                                : HTTPD_404_NOT_FOUND;
    httpd_resp_send_err(req, resp_err, NULL);
    log_probe(req);
    return ESP_OK;
}

/* ── Captive portal handlers (no auth — OS probe URLs) ───────────────── */

static esp_err_t captive_portal_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── Static file handler (wildcard) ──────────────────────────────────── */

static esp_err_t static_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) uri = "/index.html";

    char fpath[522]; /* "/littlefs" (9) + max URI (512) + null */
    snprintf(fpath, sizeof(fpath), "/littlefs%s", uri);

    FILE *f = fopen(fpath, "r");
    if (!f) {
        httpd_resp_send_404(req);
        log_probe(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, mime_for(fpath));
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── WebSocket ────────────────────────────────────────────────────────── */

esp_err_t web_server_broadcast(const char *json_payload) {
    if (!s_server) return ESP_ERR_INVALID_STATE;
    size_t client_count = 7;
    int fds[7];
    if (httpd_get_client_list(s_server, &client_count, fds) != ESP_OK) return ESP_FAIL;

    for (size_t i = 0; i < client_count; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
        httpd_ws_frame_t frame = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json_payload,
            .len     = strlen(json_payload),
        };
        httpd_ws_send_frame_async(s_server, fds[i], &frame);
    }
    return ESP_OK;
}

static void ws_tx_task(void *arg) {
    char *msg;
    while (1) {
        if (xQueueReceive(s_ws_queue, &msg, portMAX_DELAY) == pdTRUE) {
            web_server_broadcast(msg);
            free(msg);
        }
    }
}

static void on_event_bus(const char *event, const char *json_data, void *ctx) {
    char buf[640];
    const char *data = json_data ? json_data : "null";
    int n = snprintf(buf, sizeof(buf), "{\"event\":\"");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s", event);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\",\"data\":");
    snprintf(buf + n, sizeof(buf) - (size_t)n, "%s}", data);
    char *copy = strdup(buf);
    if (copy) {
        if (xQueueSend(s_ws_queue, &copy, pdMS_TO_TICKS(50)) != pdTRUE) {
            free(copy);
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        if (!check_auth(req)) {
            send_401(req);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    uint8_t buf[256] = {0};
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = buf};
    httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    return ESP_OK;
}

/* ── API: /api/info ───────────────────────────────────────────────────── */

static esp_err_t api_info_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    char device_name[64];
    config_get_str("device_name", device_name, sizeof(device_name));

    char ip_str[16]    = "N/A";
    char state_str[16] = "ap_mode";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            strlcpy(state_str, "connected", sizeof(state_str));
        }
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"device_name\":\"%s\",\"wifi_state\":\"%s\",\"ip\":\"%s\","
             "\"free_heap\":%lu,\"uptime_s\":%lld}",
             device_name, state_str, ip_str,
             (unsigned long)esp_get_free_heap_size(),
             esp_timer_get_time() / 1000000LL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── API: /api/config ─────────────────────────────────────────────────── */

static esp_err_t api_config_get_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    char device_name[64], http_user[64];
    config_get_str("device_name", device_name, sizeof(device_name));
    config_get_str("http_user",   http_user,   sizeof(http_user));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddStringToObject(root, "http_user",   http_user);
    cJSON_AddStringToObject(root, "http_pass",   "***");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_config_post_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    char *body = recv_body(req, 512);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "device_name")) && cJSON_IsString(v))
        config_set_str("device_name", v->valuestring);
    if ((v = cJSON_GetObjectItem(root, "http_user")) && cJSON_IsString(v))
        config_set_str("http_user", v->valuestring);
    if ((v = cJSON_GetObjectItem(root, "http_pass")) && cJSON_IsString(v))
        config_set_str("http_pass", v->valuestring);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

/* ── API: /api/wifi/creds ─────────────────────────────────────────────── */

static esp_err_t api_wifi_creds_get_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    wifi_cred_t creds[WIFI_MAX_CREDS];
    uint8_t count = 0;
    config_get_wifi_creds(creds, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", creds[i].ssid);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "creds", arr);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_wifi_creds_post_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    char *body = recv_body(req, 256);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pwd  = cJSON_GetObjectItem(root, "pwd");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pwd)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }
    config_add_wifi_cred(ssid->valuestring, pwd->valuestring);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t api_wifi_creds_delete_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    char *body = recv_body(req, 128);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    if (cJSON_IsString(ssid)) config_remove_wifi_cred(ssid->valuestring);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

/* ── API: /api/wifi/connect ───────────────────────────────────────────── */

static esp_err_t api_wifi_connect_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    event_bus_post("wifi_reconnect", NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

/* ── API: /api/led ────────────────────────────────────────────────────── */

static esp_err_t api_led_get_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    uint8_t r = 0, g = 0, b = 0, w = 0, brightness = 255;
    bool on = false;
    led_get_state(&r, &g, &b, &w, &brightness, &on);

    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"r\":%d,\"g\":%d,\"b\":%d,\"w\":%d,\"brightness\":%d,\"on\":%s}",
             r, g, b, w, brightness, on ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t api_led_post_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    char *body = recv_body(req, 128);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *br_item = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(br_item))
        led_set_brightness((uint8_t)br_item->valueint);

    cJSON *on_item = cJSON_GetObjectItem(root, "on");
    if (on_item && cJSON_IsBool(on_item) && !cJSON_IsTrue(on_item)) {
        led_set_off();
    } else {
        cJSON *r = cJSON_GetObjectItem(root, "r");
        cJSON *g = cJSON_GetObjectItem(root, "g");
        cJSON *b = cJSON_GetObjectItem(root, "b");
        cJSON *w = cJSON_GetObjectItem(root, "w");
        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b)) {
            uint8_t wv = cJSON_IsNumber(w) ? (uint8_t)w->valueint : 0;
            led_set_rgbw((uint8_t)r->valueint, (uint8_t)g->valueint, (uint8_t)b->valueint, wv);
        }
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

/* ── API: /api/led/config ─────────────────────────────────────────────── */

static esp_err_t api_led_config_get_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    uint32_t gpio = 48, count = 1;
    char proto[8] = "GRB";
    config_get_u32("led_gpio",  &gpio);
    config_get_u32("led_count", &count);
    config_get_str("led_proto", proto, sizeof(proto));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "gpio",  (double)gpio);
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddStringToObject(root, "proto", proto);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_led_config_post_handler(httpd_req_t *req) {
    AUTH_CHECK(req);
    char *body = recv_body(req, 128);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL); return ESP_OK; }

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "gpio"))  && cJSON_IsNumber(v))
        config_set_u32("led_gpio",  (uint32_t)v->valueint);
    if ((v = cJSON_GetObjectItem(root, "count")) && cJSON_IsNumber(v))
        config_set_u32("led_count", (uint32_t)v->valueint);
    if ((v = cJSON_GetObjectItem(root, "proto")) && cJSON_IsString(v))
        config_set_str("led_proto", v->valuestring);
    cJSON_Delete(root);

    led_controller_reinit();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

/* ── Init / Start / Stop ──────────────────────────────────────────────── */

esp_err_t web_server_init(void) {
    esp_err_t ret = mount_littlefs();
    if (ret != ESP_OK) return ret;

    s_ws_queue = xQueueCreate(16, sizeof(char *));
    if (!s_ws_queue) return ESP_ERR_NO_MEM;

    xTaskCreate(ws_tx_task, "ws_tx", 4096, NULL, 5, NULL);
    return event_bus_subscribe(on_event_bus, NULL);
}

esp_err_t web_server_start(void) {
    if (s_server) return ESP_OK;

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets  = 7;
    cfg.max_uri_handlers  = 24;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register handlers — specific routes before the catch-all.
       Captive portal probes come first and require no auth. */
    static const httpd_uri_t captive_routes[] = {
        { .uri = "/generate_204",                   .method = HTTP_GET, .handler = captive_portal_handler },
        { .uri = "/hotspot-detect.html",            .method = HTTP_GET, .handler = captive_portal_handler },
        { .uri = "/library/test/success.html",      .method = HTTP_GET, .handler = captive_portal_handler },
        { .uri = "/ncsi.txt",                       .method = HTTP_GET, .handler = captive_portal_handler },
        { .uri = "/connecttest.txt",                .method = HTTP_GET, .handler = captive_portal_handler },
        { .uri = "/redirect",                       .method = HTTP_GET, .handler = captive_portal_handler },
        { .uri = "/canonical.html",                 .method = HTTP_GET, .handler = captive_portal_handler },
    };
    for (size_t i = 0; i < sizeof(captive_routes) / sizeof(captive_routes[0]); i++) {
        httpd_register_uri_handler(s_server, &captive_routes[i]);
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/ws",                .method = HTTP_GET,    .handler = ws_handler,                   .is_websocket = true },
        { .uri = "/api/info",          .method = HTTP_GET,    .handler = api_info_handler              },
        { .uri = "/api/config",        .method = HTTP_GET,    .handler = api_config_get_handler        },
        { .uri = "/api/config",        .method = HTTP_POST,   .handler = api_config_post_handler       },
        { .uri = "/api/wifi/creds",    .method = HTTP_GET,    .handler = api_wifi_creds_get_handler    },
        { .uri = "/api/wifi/creds",    .method = HTTP_POST,   .handler = api_wifi_creds_post_handler   },
        { .uri = "/api/wifi/creds",    .method = HTTP_DELETE, .handler = api_wifi_creds_delete_handler },
        { .uri = "/api/wifi/connect",  .method = HTTP_POST,   .handler = api_wifi_connect_handler      },
        { .uri = "/api/led",           .method = HTTP_GET,    .handler = api_led_get_handler           },
        { .uri = "/api/led",           .method = HTTP_POST,   .handler = api_led_post_handler          },
        { .uri = "/api/led/config",    .method = HTTP_GET,    .handler = api_led_config_get_handler    },
        { .uri = "/api/led/config",    .method = HTTP_POST,   .handler = api_led_config_post_handler   },
        { .uri = "/",                  .method = HTTP_GET,    .handler = static_handler                },
        { .uri = "/*",                 .method = HTTP_GET,    .handler = static_handler                },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND,          err_probe_handler);
    httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED, err_probe_handler);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void) {
    if (!s_server) return ESP_OK;
    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    return ret;
}
