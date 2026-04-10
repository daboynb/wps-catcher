idf_component_register(
    SRCS "main.c" "wifi_manager.c" "server.c" "server_api.c" "dns.c" "config.c"
         "wifi_wps.c" "wifi_saved.c"
    INCLUDE_DIRS "../include"
    REQUIRES nvs_flash esp_wifi esp_http_server esp_netif json spiffs wpa_supplicant
)

spiffs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)
