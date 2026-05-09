# Plan: ESP32-S3 Web Device Framework

## Context

The repo currently has only a stub `app_main()`. The goal is to implement the full framework from `Spec.md`, updated with the following requirements:
- AP always active (APSTA mode) — device reachable at `192.168.4.1` even when STA connected
- Multiple WiFi credentials stored (up to 8), tried in RSSI order on scan
- Pico.css frontend (bundled, no CDN), SPA with burger-menu nav
- WiFi management page (add / remove credentials)

---

## Architecture

```
main
 ├── config_manager   →  nvs_flash
 ├── event_bus        →  esp_event
 ├── led_controller   →  event_bus, esp_led_strip* (→ esp_driver_rmt)
 ├── web_server       →  esp_http_server, esp_littlefs*, event_bus, config_manager, led_controller
 └── wifi_manager     →  esp_wifi, esp_netif, esp_event, config_manager, web_server, event_bus

* managed components
```

`web_server` is initialized before `wifi_manager` so it's ready when `wifi_manager` calls `web_server_start()`.

---

## Files to Create / Modify

### Modified
- `CMakeLists.txt` — rename project to `web_device_framework`
- `main/CMakeLists.txt` — add REQUIRES for all four custom components + system deps
- `main/main.c` — implement `app_main()` init sequence

### New
```
partitions.csv
sdkconfig.defaults
components/
  config_manager/
    CMakeLists.txt
    include/config_manager.h
    config_manager.c
  event_bus/
    CMakeLists.txt
    include/event_bus.h
    event_bus.c
  led_controller/
    CMakeLists.txt
    idf_component.yml          ← declares espressif/led_strip dependency
    Kconfig                    ← LED_CONTROLLER_GPIO (default 48)
    include/led_controller.h
    led_controller.c
  web_server/
    CMakeLists.txt
    idf_component.yml          ← declares joltwallet/esp_littlefs dependency
    include/web_server.h
    web_server.c
  wifi_manager/
    CMakeLists.txt
    include/wifi_manager.h
    wifi_manager.c
main/
  CMakeLists.txt
  idf_component.yml            ← declares espressif/mdns dependency
  main.c
web/
  index.html                   ← SPA: dashboard + wifi-config sections
  app.js
  style.css                    ← Pico.css overrides / layout glue only
  pico.min.css                 ← Pico CSS bundled (no CDN)
```

---

## Implementation Details

### `partitions.csv`
```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x5000
otadata,  data, ota,     0xe000,  0x2000
phy_init, data, phy,     0x10000, 0x1000
factory,  app,  factory, 0x20000, 0x180000
littlefs, data, spiffs,  ,        0x80000
```

### `sdkconfig.defaults`
```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESP_LITTLEFS_ENABLE=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y
```

---

### `components/config_manager`

**Public API (`config_manager.h`):**
```c
esp_err_t config_manager_init(void);
esp_err_t config_get_str(const char *key, char *out, size_t max_len);
esp_err_t config_set_str(const char *key, const char *value);
esp_err_t config_get_u32(const char *key, uint32_t *out);
esp_err_t config_set_u32(const char *key, uint32_t value);
esp_err_t config_get_blob(const char *key, void *out, size_t *len);
esp_err_t config_set_blob(const char *key, const void *data, size_t len);
```

**`config_manager.c`:**
- Opens NVS handle namespace `cfg` (RW) in `init()`, keeps handle open.
- `init()` writes defaults for missing string keys: `device_name="esp32-device"`, `http_user="admin"`, `http_pass="admin"`.
- No default WiFi credentials written (empty blob is fine — wifi_manager handles absent key).
- `config_get_blob` / `config_set_blob` wrap `nvs_get_blob` / `nvs_set_blob` + commit.

**CMakeLists.txt:**
```cmake
idf_component_register(SRCS "config_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash)
```

---

### `components/led_controller`

Controls a single WS2812 LED via the `espressif/led_strip` managed component (which uses `esp_driver_rmt` internally — already bundled).

**`Kconfig`:**
```kconfig
menu "LED Controller"
config LED_CONTROLLER_GPIO
    int "WS2812 data GPIO number"
    default 48
    help
        GPIO pin connected to the WS2812 DIN line.
endmenu
```

**Public API (`led_controller.h`):**
```c
esp_err_t led_controller_init(void);
esp_err_t led_set_rgb(uint8_t r, uint8_t g, uint8_t b);
esp_err_t led_set_off(void);
esp_err_t led_get_state(uint8_t *r, uint8_t *g, uint8_t *b, bool *on);
```

**`led_controller.c`:**
- `led_controller_init()`: configures `led_strip_config_t` + `led_strip_rmt_config_t`, calls `led_strip_new_rmt_device()`. Subscribes to event_bus for `"led_set"` events (JSON `{"r":x,"g":y,"b":z}` or `{"on":false}`).
- `led_set_rgb()`: calls `led_strip_set_pixel()` + `led_strip_refresh()`, stores state, posts `event_bus_post("led_state_changed", "{\"r\":x,\"g\":y,\"b\":z,\"on\":true}")`.
- `led_set_off()`: calls `led_strip_clear()`, posts `event_bus_post("led_state_changed", "{\"on\":false}")`.
- Module-level statics: `led_strip_handle_t strip`, `uint8_t cur_r/g/b`, `bool cur_on`.

**`idf_component.yml`:**
```yaml
dependencies:
  espressif/led_strip: ">=2.5.0"
  idf: ">=5.0.0"
```

**CMakeLists.txt:**
```cmake
idf_component_register(SRCS "led_controller.c"
    INCLUDE_DIRS "include"
    REQUIRES event_bus)
```

---

### `components/event_bus`

**`event_bus.c`:**
- Owns a private `esp_event_loop_handle_t` (dedicated task, stack 4096, priority 5).
- Event data struct: `typedef struct { char event[64]; char json_data[512]; } event_bus_msg_t;`
- Up to 8 subscribers held in a static array of `{handler, ctx}` pairs.
- `event_bus_post()` fills a `event_bus_msg_t`, calls `esp_event_post_to()` — safe from any task.
- Single internal esp_event handler unpacks the struct and fans out to all registered user callbacks.

**CMakeLists.txt:**
```cmake
idf_component_register(SRCS "event_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event)
```

---

### `components/wifi_manager`

#### Credential storage (defined in `config_manager.h`, used by both wifi_manager and web_server)
```c
#define WIFI_MAX_CREDS 8

typedef struct {
    char ssid[64];
    char pwd[32];
} wifi_cred_t;

// Added to config_manager.h / config_manager.c:
esp_err_t config_get_wifi_creds(wifi_cred_t *out, uint8_t *count);
esp_err_t config_set_wifi_creds(const wifi_cred_t *creds, uint8_t count);
// Helpers that web_server calls directly:
esp_err_t config_add_wifi_cred(const char *ssid, const char *pwd);
esp_err_t config_remove_wifi_cred(const char *ssid);
```
Stored as NVS blob `wifi_creds` (array of `wifi_cred_t`) + u32 `wifi_cred_n` for count.

#### WiFi state machine (always APSTA)

```
wifi_manager_init()
  │  esp_netif_create_default_wifi_sta()
  │  esp_netif_create_default_wifi_ap()
  │  esp_wifi_init() + register event handlers
  └─► state = DISCONNECTED

wifi_manager_start()
  │  set WIFI_MODE_APSTA
  │  configure & start AP  (SSID: {device_name}-{MAC[3:6]}, open, 192.168.4.1)
  │  → state = AP_MODE, invoke callback
  └─► if any creds stored: esp_wifi_scan_start(non-blocking)

WIFI_EVENT_SCAN_DONE
  │  esp_wifi_scan_get_ap_records()
  │  match against stored creds, sort matched by RSSI desc
  └─► if matches: attempt first → state = CONNECTING; else: schedule rescan after 30s

WIFI_EVENT_STA_DISCONNECTED
  │  if retry_idx < match_count: try next matched credential
  └─► else: schedule rescan (exponential backoff: 2→4→8→16→30s cap), reset retry_idx

IP_EVENT_STA_GOT_IP
  └─► state = CONNECTED, invoke callback, call web_server_start() (idempotent)

Note: AP stays up throughout. web_server_start() is idempotent (no-op if already running).
```

**CMakeLists.txt:**
```cmake
idf_component_register(SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event config_manager web_server)
```

---

### `components/web_server`

#### Endpoints

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | `/*` | Yes | LittleFS static (`/` → `index.html`) |
| GET | `/ws` | On upgrade | WebSocket |
| GET | `/api/info` | Yes | Device info JSON |
| GET | `/api/config` | Yes | Config keys (redacts `http_pass`) |
| POST | `/api/config` | Yes | Update config keys |
| GET | `/api/wifi/creds` | Yes | List stored SSIDs (no passwords) |
| POST | `/api/wifi/creds` | Yes | Add credential `{ssid, pwd}` |
| DELETE | `/api/wifi/creds` | Yes | Remove credential `{ssid}` |
| POST | `/api/wifi/connect` | Yes | Trigger rescan + reconnect |
| GET | `/api/led` | Yes | Current LED state `{r,g,b,on}` |
| POST | `/api/led` | Yes | Set LED `{r,g,b}` or `{on:false}` |

`web_server_start()` is idempotent — returns `ESP_OK` immediately if server already running.

#### Internals
- `httpd_config_t.max_open_sockets = 7` (4 WS + 3 reserved).
- Basic Auth helper decodes `Authorization: Basic` header on every non-WS request; returns 401 on failure.
- Static file handler: resolves `/littlefs<uri>`, detects MIME by extension (`.html .js .css .json .ico .png`), streams via `httpd_resp_send_chunk`, 404 if not found.
- WebSocket broadcast: `httpd_get_client_list()` → filter `HTTPD_WS_CLIENT_WEBSOCKET` → `httpd_ws_send_frame_async()` per fd; remove stale fds on error.
- WS tx task: event_bus subscriber posts JSON strings to a `QueueHandle_t`; dedicated task drains queue and calls `web_server_broadcast()`.

**`idf_component.yml`:**
```yaml
dependencies:
  joltwallet/esp_littlefs: ">=1.14.0"
  idf: ">=5.0.0"
```

**CMakeLists.txt:**
```cmake
idf_component_register(SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server event_bus config_manager esp_timer esp_netif led_controller)
```
(`esp_littlefs` pulled in as managed component; no wifi_manager dependency — see below.)

#### Circular dependency resolution

`wifi_manager` calls `web_server_start()`; `web_server` credential endpoints need to update creds and trigger reconnect. Rather than a mutual REQUIRES:

1. **Credential storage** lives entirely in `config_manager`: `wifi_cred_t` struct defined in `config_manager.h`; `config_get_wifi_creds` / `config_set_wifi_creds` wrap the NVS blob. Both `web_server` and `wifi_manager` read/write creds through `config_manager` only.

2. **Reconnect trigger** uses `event_bus` (in-process only — no network required): `POST /api/wifi/connect` writes updated creds to config_manager then calls `event_bus_post("wifi_reconnect", NULL)`. The event_bus is a custom `esp_event_loop` running entirely on the device (a dedicated FreeRTOS task); it has no dependency on WiFi or WebSocket connectivity. `wifi_manager` subscribes to event_bus at init and re-runs the scan/connect flow when the event fires. The HTTP request that triggers this arrives over either AP or STA — both are always up.

Result: `web_server` depends on `event_bus` + `config_manager` (no wifi_manager); `wifi_manager` depends on `web_server` + `event_bus` + `config_manager`. No cycle.

---

### `main/main.c`

```c
static void on_wifi_state_change(wifi_state_t state) {
    // log only; web_server_start() called internally by wifi_manager
}

static void mdns_setup(void) {
    char device_name[64];
    config_get_str("device_name", device_name, sizeof(device_name));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(device_name));
    ESP_ERROR_CHECK(mdns_instance_name_set(device_name));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    // device reachable at <device_name>.local on both AP and STA interfaces
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
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
```

mDNS is initialized once; the ESP-IDF mDNS component automatically announces on all active netif interfaces (AP and STA). Managed component: add `espressif/mdns` to `main/idf_component.yml`:
```yaml
dependencies:
  espressif/mdns: ">=1.3.0"
  idf: ">=5.0.0"
```

---

### Frontend (`web/`)

**Technology:** Pico.css (bundled as `pico.min.css`), vanilla JS, no build step.

**SPA layout (`index.html`):**
```
<nav>  logo | ☰ burger button
<aside id="nav-drawer">  [Home] [WiFi Config]  (slides in from left)
<main>
  <section id="page-home">    ← active by default
  <section id="page-wifi">    ← hidden by default, shown on nav
```

**Home page (`#page-home`):**
- Status bar: WiFi state, current IP, free heap, uptime (updated every 10 s from `/api/info`).
- **LED controls:** `<input type="color">` for RGB colour, On/Off toggle button. On load, `GET /api/led` sets initial state. Changes call `POST /api/led`. Incoming WebSocket `led_state_changed` events update the picker and toggle in real time.
- Live event log: scrollable `<article>` fed by all WebSocket messages.

**WiFi Config page (`#page-wifi`):**
- Heading: "Saved Networks"
- Table of stored SSIDs (no passwords shown), each row with a **Remove** button.
- "Add Network" form: SSID input + Password input + **Save** button.
- "Connect Now" button → `POST /api/wifi/connect` (triggers firmware rescan).
- All data fetched from `GET /api/wifi/creds` on page reveal; list refreshes after add/remove.

**`app.js` responsibilities:**
1. Burger button toggles `aria-hidden` on `<aside>`; nav links swap active `<section>`, lazy-load its data.
2. WebSocket: `ws://${location.host}/ws` — on message, parse `{event, data}`, prepend to event log. Dispatch `led_state_changed` to update LED controls in real time.
3. `/api/info` polled every 10 s — updates status bar.
4. LED controls: `GET /api/led` on page load sets picker + toggle state. Colour picker `input` / toggle click → `POST /api/led`.
5. WiFi page: `loadWifiPage()` calls `GET /api/wifi/creds` → renders table.
6. Add form: `POST /api/wifi/creds` → `loadWifiPage()`.
7. Remove: `DELETE /api/wifi/creds` → `loadWifiPage()`.
8. Connect Now: `POST /api/wifi/connect` → show toast "Reconnecting…".
9. HTTP credentials stored in `sessionStorage`; on first 401, prompt user and retry.

**`style.css`:** Layout glue only (drawer slide-in, nav overlay backdrop, event log `max-height` + `overflow-y: auto`). All typography/color/form/button styling from Pico.css.

---

## Build & Verification

1. `idf_component.yml` files declare `joltwallet/esp_littlefs` (web_server), `espressif/mdns` (main). First `idf.py build` fetches managed components automatically.
2. `idf.py set-target esp32s3 && idf.py build` — should compile cleanly.
3. Flash firmware + LittleFS image.
   1. **AP always up:** connect to `esp32-device-XXXXXX` → `http://192.168.4.1` or `http://esp32-device.local` → dashboard loads.
4. **mDNS:** from a connected client `ping esp32-device.local` resolves to device IP.
5. **WiFi page:** add a credential → "Connect Now" → device scans → connects → IP shown in status bar; AP still reachable; `esp32-device.local` resolves on STA network too.
6. **Multi-cred test:** store two SSIDs — device connects to the one with stronger RSSI.
7. **Auth test:** curl without `Authorization` → 401; with `admin:admin` → 200.
8. **WebSocket test:** open browser console, verify WS connected; `event_bus_post` from a test task → JSON frame appears in dashboard event log.