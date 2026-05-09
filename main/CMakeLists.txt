idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES config_manager event_bus led_controller web_server wifi_manager
             nvs_flash esp_netif esp_event)

littlefs_create_partition_image(littlefs ${CMAKE_CURRENT_SOURCE_DIR}/../web FLASH_IN_PROJECT)
