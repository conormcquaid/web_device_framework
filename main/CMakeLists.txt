idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES config_manager event_bus led_controller web_server wifi_manager
             nvs_flash esp_netif esp_event)
