# ESP32-S3 Web Device Framework — Specification

## Context

This is a reusable firmware framework for ESP32-S3 (via ESP-IDF v5.x) that exposes a web interface over WiFi. The goal is a clean starting point that any project can extend: connect the device to WiFi, serve a real-time HTML dashboard over WebSocket, and persist configuration in NVS. The project currently has only an empty `app_main()` in `main/main.c`.

---

## Architecture Overview

Four custom components wired together in `main.c`:

```
main
 ├── config_manager   (NVS key-value config)
 ├── wifi_manager     (STA + AP fallback)
 ├── web_server       (HTTP + WebSocket + Basic Auth + LittleFS static files)
 └── event_bus        (internal pub/sub → WebSocket broadcast)
```

---

## Component Specifications

### 1. `config_manager`

**Purpose:** Thin wrapper around NVS. All other components call this for persistent storage.

**API (`include/config_manager.h`):**
```c
esp_err_t config_manager_init(void);
esp_err_t config_get_str(const char *key, char *out, size_t max_len);
esp_err_t config_set_str(const char *key, const char *value);
esp_err_t config_get_u32(const char *key, uint32_t *out);
esp_err_t config_set_u32(const char *key, uint32_t value);
```

**NVS namespace:** `cfg`

**Default keys:**
| Key | Default | Notes |
|---|---|---|
| `wifi_ssid` | `""` | STA SSID |
| `wifi_pass` | `""` | STA password |
| `device_name` | `"esp32-device"` | Used in AP SSID and HTTP server header |
| `http_user` | `"admin"` | Basic Auth username |
| `http_pass` | `"admin"` | Basic Auth password |

---

### 2. `wifi_manager`

**Purpose:** Manages WiFi connectivity. Attempts STA; falls back to AP if connection fails.

**API (`include/wifi_manager.h`):**
```c
typedef enum {
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE,
    WIFI_STATE_DISCONNECTED,
} wifi_state_t;

typedef void (*wifi_state_cb_t)(wifi_state_t state);

esp_err_t wifi_manager_init(wifi_state_cb_t callback);
esp_err_t wifi_manager_start(void);
wifi_state_t wifi_manager_get_state(void);
void wifi_manager_set_credentials(const char *ssid, const char *pass);
```

**Behavior:**
1. Load `wifi_ssid` / `wifi_pass` from `config_manager`.
2. If credentials exist, attempt STA connection. Retry up to 5 times with 2 s back-off.
3. On success → `WIFI_STATE_CONNECTED`; invoke callback; start web server.
4. On all retries exhausted → switch to AP mode → `WIFI_STATE_AP_MODE`; invoke callback; start web server.
5. AP SSID: `{device_name}-{last 3 bytes of MAC}`, open (no password), IP `192.168.4.1`.
6. If STA disconnects after initial connection, attempt reconnection indefinitely with exponential back-off (2 s → 4 s → 8 s → cap at 30 s).

**ESP-IDF APIs used:** `esp_wifi`, `esp_event`, `esp_netif`

---

### 3. `event_bus`

**Purpose:** Lightweight in-process pub/sub. The `web_server` subscribes and broadcasts events to all WebSocket clients.

**API (`include/event_bus.h`):**
```c
typedef void (*event_bus_handler_t)(const char *event, const char *json_data, void *ctx);

esp_err_t event_bus_init(void);
esp_err_t event_bus_subscribe(event_bus_handler_t handler, void *ctx);
esp_err_t event_bus_post(const char *event, const char *json_data);
```

**Wire format (WebSocket payload):**
```json
{"event": "sensor_update", "data": {"temp": 23.5}}
```

**Implementation notes:**
- Backed by `esp_event_loop` (custom loop, not the default system loop).
- `event_bus_post` is safe to call from any task.
- Handler is invoked on the event loop task; web_server must forward to a dedicated WebSocket tx task.

---

### 4. `web_server`

**Purpose:** HTTP server with Basic Auth, static file serving from LittleFS, and WebSocket endpoint.

**API (`include/web_server.h`):**
```c
esp_err_t web_server_init(void);
esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);
esp_err_t web_server_broadcast(const char *json_payload);
```

**Endpoints:**

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/*` | Yes | Serve file from LittleFS (`/littlefs<path>`). `GET /` → `index.html` |
| `GET` | `/ws` | No† | WebSocket upgrade |
| `GET` | `/api/info` | Yes | Device info JSON |
| `GET` | `/api/config` | Yes | Read config keys (redacts `http_pass`) |
| `POST` | `/api/config` | Yes | Update config keys (JSON body) |
| `POST` | `/api/wifi` | Yes | Update WiFi credentials + trigger reconnect |

†WebSocket connections use Basic Auth on the upgrade request only (HTTP header).

**Basic Auth implementation:**
- Decode `Authorization: Basic <b64>` header on every request.
- Compare against `http_user`/`http_pass` from `config_manager`.
- Return 401 + `WWW-Authenticate: Basic realm="ESP32"` on failure.

**LittleFS static serving:**
- Mount point: `/littlefs`
- MIME types detected by file extension: `.html`, `.js`, `.css`, `.json`, `.ico`, `.png`
- 404 response if file not found (do not fall back to `index.html`)

**WebSocket handling:**
- Maximum 4 simultaneous clients (bounded by `httpd_config.max_open_sockets`).
- On `event_bus` event: serialize to JSON and call `web_server_broadcast()`.
- Broadcast iterates all open WS fds and calls `httpd_ws_send_frame_async`.
- Disconnected clients are cleaned up on next broadcast attempt.

**`GET /api/info` response:**
```json
{
  "device_name": "esp32-device",
  "wifi_state": "connected",
  "ip": "192.168.1.42",
  "free_heap": 234512,
  "uptime_s": 3600
}
```

**ESP-IDF APIs used:** `esp_http_server` (includes WS support), `esp_littlefs`

---

## Filesystem & Partition Table

**Custom `partitions.csv`:**
```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
phy_init, data, phy,     0x10000, 0x1000
factory,  app,  factory, 0x20000, 0x180000
littlefs, data, spiffs,  ,        0x80000
```
(Assumes 4 MB flash; adjust factory/littlefs sizes for 8 MB boards.)

**`sdkconfig.defaults`:**
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_LITTLEFS_ENABLE=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y
```

---

## Project File Structure

```
web_device_framework/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   └── main.c                  ← init sequence: config → wifi → event_bus → web_server
├── components/
│   ├── config_manager/
│   │   ├── CMakeLists.txt
│   │   ├── include/config_manager.h
│   │   └── config_manager.c
│   ├── wifi_manager/
│   │   ├── CMakeLists.txt
│   │   ├── include/wifi_manager.h
│   │   └── wifi_manager.c
│   ├── event_bus/
│   │   ├── CMakeLists.txt
│   │   ├── include/event_bus.h
│   │   └── event_bus.c
│   └── web_server/
│       ├── CMakeLists.txt
│       ├── include/web_server.h
│       └── web_server.c
└── web/                        ← frontend source (built separately, flashed to LittleFS)
    ├── index.html
    ├── app.js                  ← connects WebSocket, renders events
    └── style.css
```

---

## `main.c` Init Sequence

```c
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(config_manager_init());
    ESP_ERROR_CHECK(event_bus_init());
    ESP_ERROR_CHECK(web_server_init());
    // web_server subscribes to event_bus internally

    ESP_ERROR_CHECK(wifi_manager_init(on_wifi_state_change));
    ESP_ERROR_CHECK(wifi_manager_start());
    // wifi_manager calls web_server_start() once IP is obtained
}
```

---

## Frontend (web/)

**`index.html`:** Single-page app. Includes `style.css` and `app.js`.

**`app.js` responsibilities:**
1. Open `ws://<host>/ws` (with Basic Auth credentials in URL or pre-auth header).
2. Listen for JSON event messages; dispatch to registered UI handlers by `event` name.
3. Provide a `fetchConfig()` / `saveConfig()` helper that calls `/api/config`.
4. Show WiFi/connection status from `/api/info` (poll every 10 s).

**`style.css`:** Minimal responsive styles; no external dependencies (embedded device, no CDN access).

---

## Verification / Testing

1. **Build:** `idf.py set-target esp32s3 && idf.py build` — should compile cleanly.
2. **Flash firmware + LittleFS:** `idf.py flash` + `idf.py -p /dev/ttyUSB0 littlefs-flash` (using `idf_component_manager` or `esptool`).
3. **AP mode test:** First boot with no credentials → device creates AP `esp32-device-XXXXXX` → browse to `http://192.168.4.1` → config page loads → enter WiFi credentials → POST `/api/wifi`.
4. **STA mode test:** Reboot after saving credentials → device connects to network → browse to IP shown in serial log → dashboard loads.
5. **WebSocket test:** Open browser console → verify WS connection at `/ws` → post a test event from device console or a test task → confirm JSON event received in browser.
6. **Auth test:** Request any endpoint without `Authorization` header → expect 401. With correct credentials → expect 200.
7. **Serial monitor:** `idf.py monitor` — log WiFi state transitions, HTTP requests, WS connect/disconnect, event bus posts.
