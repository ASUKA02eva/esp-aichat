# WiFi 连接管理（STA 模式）
idf_component_register(SRCS "wifi_manager.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif nvs_flash)
