idf_component_register(SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server event_bus config_manager esp_timer esp_netif led_controller json
    PRIV_REQUIRES mbedtls)
