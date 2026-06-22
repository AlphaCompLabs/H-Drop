idf_component_register(
    SRCS "hdrop_ota.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_server app_update nvs_flash hdrop_motor
)
